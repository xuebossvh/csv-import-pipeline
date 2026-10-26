// Independent, streaming check for the exercise's supplied sample_1gb.csv.
// Usage: node tools/verify_archive.cjs input.csv errors.jsonl verification.json
const fs = require('fs');
const crypto = require('crypto');
const path = require('path');
const { performance } = require('perf_hooks');

async function main() {

const [input, archive, output] = process.argv.slice(2);
if (!input || !archive || !output) {
  console.error('Usage: node tools/verify_archive.cjs input.csv errors.jsonl verification.json');
  process.exit(1);
}
const inputDigest = crypto.createHash('sha256');
const archiveDigest = crypto.createHash('sha256');
const metrics = { bytes: 0, endedWithLf: false, maxLineBytes: 0 };
async function* lines(file, digest, info) {
  let tail = Buffer.alloc(0);
  for await (const chunk of fs.createReadStream(file, { highWaterMark: 1024 * 1024 })) {
    digest.update(chunk);
    if (info) { info.bytes += chunk.length; info.endedWithLf = chunk.at(-1) === 10; }
    const block = tail.length ? Buffer.concat([tail, chunk]) : chunk;
    let begin = 0, end;
    while ((end = block.indexOf(10, begin)) !== -1) {
      let row = block.subarray(begin, end);
      if (row.at(-1) === 13) row = row.subarray(0, row.length - 1);
      if (info) info.maxLineBytes = Math.max(info.maxLineBytes, row.length);
      yield row;
      begin = end + 1;
    }
    tail = Buffer.from(block.subarray(begin));
    if (tail.length > 1024 * 1024) throw new Error('Fixture line exceeds 1 MiB');
  }
  if (tail.length) {
    if (info) info.maxLineBytes = Math.max(info.maxLineBytes, tail.length);
    yield tail;
  }
}
function requireCondition(value, message) { if (!value) throw new Error(message); }
const byteString = s => {
  requireCondition(typeof s === 'string' && [...s].every(c => c.charCodeAt(0) <= 255),
                   'Archive string is not a reversible byte string');
  return Buffer.from(s, 'latin1');
};
const started = performance.now();
const errors = lines(archive, archiveDigest)[Symbol.asyncIterator]();
let physicalLine = 0, imported = 0, rejected = 0, firstError = 0, lastError = 0;
for await (const bytes of lines(input, inputDigest, metrics)) {
  physicalLine++;
  if (physicalLine === 1) {
    requireCondition(bytes.equals(Buffer.from('id,name,age,score,active')), 'Unexpected header');
    continue;
  }
  const fields = bytes.toString('latin1').split(',');
  const id = physicalLine - 1;
  requireCondition(fields.length === 5 && fields[0] === String(id), 'Unexpected id/columns at ' + physicalLine);
  requireCondition(fields[1] === ('User' + String(id).padStart(8, '0')).padEnd(78, 'x'),
                   'Unexpected name at ' + physicalLine);
  requireCondition(/^-?\d+\.\d$/.test(fields[3]) && /^(true|false)$/.test(fields[4]),
                   'Unexpected score/active at ' + physicalLine);
  if (id % 1000 !== 0) {
    requireCondition(/^-?\d+$/.test(fields[2]), 'Unexpected invalid age at ' + physicalLine);
    imported++;
  } else {
    requireCondition(fields[2] === 'N/A', 'Expected N/A at ' + physicalLine);
    const next = await errors.next();
    requireCondition(!next.done, 'Missing archived error at ' + physicalLine);
    const error = JSON.parse(next.value.toString('utf8'));
    requireCondition(error.line_number === physicalLine, 'Incorrect/duplicate archive line number');
    requireCondition(byteString(error.raw_line).equals(bytes), 'Raw bytes differ at ' + physicalLine);
    requireCondition(Array.isArray(error.errors) && error.errors.length === 1, 'Unexpected error reasons');
    const reason = error.errors[0];
    requireCondition(reason.column_index === 2 && reason.column_name === 'age' &&
        reason.code === 3 && reason.raw_value === 'N/A' &&
        /integer/i.test(reason.message), 'Missing or incorrect age reason at ' + physicalLine);
    rejected++;
    firstError ||= physicalLine;
    lastError = physicalLine;
  }
  if (id % 1000000 === 0) console.error('Verified data rows: ' + id);
}
requireCondition((await errors.next()).done, 'Unexpected extra archived errors');
requireCondition(metrics.bytes === 1003898922 && metrics.endedWithLf, 'Wrong source size or missing final LF');
requireCondition(physicalLine === 10000001 && imported === 9990000 && rejected === 10000,
                 'Data accounting does not match the required fixture');
const result = {
  input, archive, inputBytes: metrics.bytes, inputSHA256: inputDigest.digest('hex'),
  archiveSHA256: archiveDigest.digest('hex'), dataRows: physicalLine - 1,
  importedRows: imported, errorRows: rejected, firstErrorLine: firstError, lastErrorLine: lastError,
  maxPhysicalLineBytes: metrics.maxLineBytes, finalNewline: metrics.endedWithLf,
  allErrorLineNumbersReasonsAndRawBytesMatch: true, rowConservation: imported + rejected === physicalLine - 1,
  verifierSeconds: (performance.now() - started) / 1000,
  note: 'Independent verification time is not import benchmark time; memory proof comes from the importer process.'
};
requireCondition(path.resolve(output) !== path.resolve(input) && path.resolve(output) !== path.resolve(archive),
                 'Verification output must differ from both inputs');
fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n', { flag: 'wx' });
console.log(JSON.stringify(result, null, 2));
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
