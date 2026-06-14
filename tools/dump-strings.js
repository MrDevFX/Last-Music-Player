const fs = require("fs");

const file = process.argv[2];
if (!file) {
  console.error("Usage: node tools/dump-strings.js <dump>");
  process.exit(2);
}

const patterns = [
  /abort/i,
  /terminate/i,
  /TrackInfo/i,
  /AudioPlayer/i,
  /MainWindow/i,
  /Last_Music_Player/i,
  /winrt/i,
  /hresult/i,
  /exception/i,
  /Microsoft Visual C/i,
  /assert/i,
  /invalid/i,
  /ucrt/i,
  /HRESULT/i,
  /0x80[0-9a-f]+/i,
  /package/i,
  /identity/i,
  /bootstrap/i,
  /runtime/i,
  /deployment/i,
  /application/i,
  /start/i,
  /xaml/i,
  /activation/i,
  /class/i,
  /registered/i,
];

const data = fs.readFileSync(file);
const found = new Set();

function consider(value) {
  if (value.length < 4 || value.length > 500) {
    return;
  }
  if (patterns.some((p) => p.test(value))) {
    found.add(value);
  }
}

let ascii = "";
for (const byte of data) {
  if (byte >= 32 && byte <= 126) {
    ascii += String.fromCharCode(byte);
  } else {
    consider(ascii);
    ascii = "";
  }
}
consider(ascii);

let wide = "";
for (let i = 0; i + 1 < data.length; i += 2) {
  const code = data[i] | (data[i + 1] << 8);
  if (code >= 32 && code <= 126) {
    wide += String.fromCharCode(code);
  } else {
    consider(wide);
    wide = "";
  }
}
consider(wide);

for (const line of [...found].sort()) {
  console.log(line);
}
