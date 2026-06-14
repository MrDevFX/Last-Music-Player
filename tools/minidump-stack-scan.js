const fs = require("fs");
const path = require("path");

const dumpPath = process.argv[2];
if (!dumpPath) {
  console.error("Usage: node tools/minidump-stack-scan.js <dump>");
  process.exit(2);
}

const data = fs.readFileSync(dumpPath);

function u32(off) { return data.readUInt32LE(off); }
function u64(off) { return Number(data.readBigUInt64LE(off)); }

function readMinidumpString(rva) {
  if (!rva || rva + 4 > data.length) return "";
  const byteLength = u32(rva);
  if (rva + 4 + byteLength > data.length) return "";
  return data.subarray(rva + 4, rva + 4 + byteLength).toString("utf16le");
}

if (u32(0) !== 0x504d444d) {
  throw new Error("Not an MDMP file");
}

const streamCount = u32(8);
const streamDirRva = u32(12);
const streams = new Map();
for (let i = 0; i < streamCount; i++) {
  const off = streamDirRva + i * 12;
  streams.set(u32(off), { size: u32(off + 4), rva: u32(off + 8) });
}

const modules = [];
const moduleStream = streams.get(4);
if (moduleStream) {
  const count = u32(moduleStream.rva);
  let off = moduleStream.rva + 4;
  for (let i = 0; i < count; i++) {
    const base = u64(off);
    const size = u32(off + 8);
    const nameRva = u32(off + 20);
    const name = readMinidumpString(nameRva);
    modules.push({ base, end: base + size, size, name, shortName: path.basename(name) });
    off += 108;
  }
}
modules.sort((a, b) => a.base - b.base);

function moduleFor(addr) {
  return modules.find((m) => addr >= m.base && addr < m.end);
}

const memoryRanges = [];
const memory64Stream = streams.get(9);
if (memory64Stream) {
  const count = Number(data.readBigUInt64LE(memory64Stream.rva));
  let baseRva = Number(data.readBigUInt64LE(memory64Stream.rva + 8));
  let desc = memory64Stream.rva + 16;
  for (let i = 0; i < count; i++) {
    const start = u64(desc);
    const size = u64(desc + 8);
    memoryRanges.push({ start, size, rva: baseRva });
    baseRva += size;
    desc += 16;
  }
}

function memoryFor(addr) {
  return memoryRanges.find((m) => addr >= m.start && addr < m.start + m.size);
}

function fmtAddr(addr) {
  return "0x" + addr.toString(16).padStart(16, "0");
}

function stackMemory(threadOff) {
  const start = u64(threadOff + 24);
  const size = u32(threadOff + 32);
  const rva = u32(threadOff + 36);
  if (!start || !size || !rva || rva + size > data.length) return null;
  return { start, size, rva };
}

function contextRegs(threadOff) {
  const contextSize = u32(threadOff + 40);
  const contextRva = u32(threadOff + 44);
  if (!contextRva || contextRva + contextSize > data.length || contextSize < 256) {
    return {};
  }
  return {
    rsp: u64(contextRva + 152),
    rbp: u64(contextRva + 160),
    rip: u64(contextRva + 248),
  };
}

const interesting = /Last_Music_Player|ucrt|vcruntime|Microsoft\.ui\.xaml|combase|kernelbase|ntdll/i;
const threadStream = streams.get(3);
if (!threadStream) {
  throw new Error("No ThreadListStream");
}

const exceptionStream = streams.get(6);
if (exceptionStream) {
  const ex = exceptionStream.rva;
  const exThreadId = u32(ex);
  const record = ex + 8;
  const code = u32(record);
  const flags = u32(record + 4);
  const address = u64(record + 16);
  const mod = moduleFor(address);
  console.log(`Exception thread=${exThreadId} code=0x${code.toString(16)} flags=0x${flags.toString(16)} address=${fmtAddr(address)} ${mod ? mod.shortName + "+0x" + (address - mod.base).toString(16) : ""}`);
  const paramCount = u32(record + 24);
  for (let i = 0; i < Math.min(paramCount, 15); i++) {
    console.log(`  param${i}=${fmtAddr(u64(record + 32 + i * 8))}`);
  }
}

const threadCount = u32(threadStream.rva);
let off = threadStream.rva + 4;
for (let i = 0; i < threadCount; i++) {
  const threadId = u32(off);
  const regs = contextRegs(off);
  const ripModule = regs.rip ? moduleFor(regs.rip) : null;
  const stack = stackMemory(off);
  console.log(`\nThread ${threadId} RIP=${fmtAddr(regs.rip || 0)} ${ripModule ? ripModule.shortName + "+0x" + (regs.rip - ripModule.base).toString(16) : ""} RSP=${fmtAddr(regs.rsp || 0)} stack=${stack ? fmtAddr(stack.start) + " size=0x" + stack.size.toString(16) : "none"}`);

  let scan = stack;
  if (!scan && regs.rsp) {
    const memory = memoryFor(regs.rsp);
    if (memory) {
      scan = memory;
      console.log(`  memory64 range ${fmtAddr(memory.start)} size=0x${memory.size.toString(16)}`);
    }
  }

  if (scan) {
    const seen = new Set();
    const stackOffset = Math.max(0, Math.min(scan.size - 8, Math.max(0, regs.rsp - scan.start)));
    const scanStart = scan.rva + stackOffset;
    const scanEnd = Math.min(scan.rva + scan.size, scanStart + 0x20000);
    for (let p = scanStart; p + 8 <= scanEnd; p += 8) {
      const addr = Number(data.readBigUInt64LE(p));
      const mod = moduleFor(addr);
      if (!mod || !interesting.test(mod.shortName)) continue;
      const stackAddr = scan.start + (p - scan.rva);
      const key = `${mod.shortName}:${addr.toString(16)}`;
      if (seen.has(key)) continue;
      seen.add(key);
      console.log(`  [${fmtAddr(stackAddr)}] ${fmtAddr(addr)} ${mod.shortName}+0x${(addr - mod.base).toString(16)}`);
      if (seen.size >= 80) break;
    }
  }

  off += 48;
}
