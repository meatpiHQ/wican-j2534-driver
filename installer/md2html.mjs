// md2html.mjs — convert USER_MANUAL.md to print-styled HTML for the
// installer's PDF (node md2html.mjs in.md out.html). Self-contained: no
// npm deps; covers exactly the markdown subset the manual uses
// (h1-h3, tables, fenced code, inline code/bold/em/links, blockquotes,
// ordered/unordered lists, hr).
import { readFileSync, writeFileSync } from 'fs';

const [, , inFile, outFile] = process.argv;
if (!inFile || !outFile) { console.error('usage: node md2html.mjs in.md out.html'); process.exit(2); }
const src = readFileSync(inFile, 'utf8').replace(/\r\n/g, '\n');

const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
function inline(s) {
  s = esc(s);
  s = s.replace(/`([^`]+)`/g, (_, c) => `<code>${c}</code>`);
  s = s.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
  s = s.replace(/(^|[\s(])\*([^*\s][^*]*)\*/g, '$1<em>$2</em>');
  s = s.replace(/\[([^\]]+)\]\(([^)]+)\)/g, '<a href="$2">$1</a>');
  return s;
}

const lines = src.split('\n');
const out = [];
let i = 0, title = 'WiCAN J2534 Manual';

while (i < lines.length) {
  const line = lines[i];

  if (line.startsWith('```')) {                       // fenced code
    const buf = [];
    i++;
    while (i < lines.length && !lines[i].startsWith('```')) buf.push(lines[i++]);
    i++;
    out.push(`<pre><code>${esc(buf.join('\n'))}</code></pre>`);
    continue;
  }
  const h = line.match(/^(#{1,3}) +(.*)$/);           // headings
  if (h) {
    const lvl = h[1].length;
    if (lvl === 1 && title === 'WiCAN J2534 Manual') title = h[2].replace(/[*`]/g, '');
    out.push(`<h${lvl}>${inline(h[2])}</h${lvl}>`);
    i++; continue;
  }
  if (/^-{3,}\s*$/.test(line)) { out.push('<hr>'); i++; continue; }
  if (line.startsWith('|')) {                         // table
    const rows = [];
    while (i < lines.length && lines[i].startsWith('|')) rows.push(lines[i++]);
    const cells = r => r.replace(/^\||\|$/g, '').split('|').map(c => inline(c.trim()));
    const head = cells(rows[0]);
    const body = rows.slice(2).map(cells);            // rows[1] = separator
    out.push('<table><thead><tr>' + head.map(c => `<th>${c}</th>`).join('') + '</tr></thead><tbody>'
      + body.map(r => '<tr>' + r.map(c => `<td>${c}</td>`).join('') + '</tr>').join('')
      + '</tbody></table>');
    continue;
  }
  if (line.startsWith('> ') || line === '>') {        // blockquote
    const buf = [];
    while (i < lines.length && (lines[i].startsWith('>') )) buf.push(lines[i++].replace(/^> ?/, ''));
    out.push(`<blockquote><p>${inline(buf.join(' '))}</p></blockquote>`);
    continue;
  }
  const li = line.match(/^(\s*)([-*]|\d+\.) +(.*)$/); // lists (with hanging continuation lines)
  if (li) {
    const ordered = /^\d+\./.test(li[2]);
    const items = [];
    while (i < lines.length) {
      const m = lines[i].match(/^(\s*)([-*]|\d+\.) +(.*)$/);
      if (m) { items.push(m[3]); i++; continue; }
      if (lines[i].match(/^\s{2,}\S/) && items.length) {          // continuation
        items[items.length - 1] += ' ' + lines[i].trim(); i++; continue;
      }
      break;
    }
    const tag = ordered ? 'ol' : 'ul';
    out.push(`<${tag}>` + items.map(t => `<li>${inline(t)}</li>`).join('') + `</${tag}>`);
    continue;
  }
  if (line.trim() === '') { i++; continue; }
  const buf = [line];                                  // paragraph
  i++;
  while (i < lines.length && lines[i].trim() !== '' && !/^(#|```|\||>|-{3,}|\s*([-*]|\d+\.) )/.test(lines[i]))
    buf.push(lines[i++]);
  out.push(`<p>${inline(buf.join(' '))}</p>`);
}

const html = `<!doctype html>
<html><head><meta charset="utf-8"><title>${esc(title)}</title><style>
  @page { margin: 18mm 16mm; }
  body { font: 10.5pt/1.5 "Segoe UI", Arial, sans-serif; color: #1a1a1a; max-width: 100%; }
  h1 { font-size: 20pt; border-bottom: 2px solid #2b6cb0; padding-bottom: 6px; }
  h2 { font-size: 14pt; color: #2b6cb0; margin-top: 1.4em; break-after: avoid; }
  h3 { font-size: 11.5pt; margin-top: 1.2em; break-after: avoid; }
  code { font: 9.5pt Consolas, monospace; background: #f0f2f5; padding: 1px 4px; border-radius: 3px; }
  pre { background: #f0f2f5; border: 1px solid #d7dce2; border-radius: 4px; padding: 8px 10px; overflow-x: hidden; white-space: pre-wrap; break-inside: avoid; }
  pre code { background: none; padding: 0; }
  table { border-collapse: collapse; width: 100%; margin: 0.7em 0; break-inside: avoid; }
  th, td { border: 1px solid #c9ced4; padding: 5px 8px; text-align: left; vertical-align: top; font-size: 9.5pt; }
  th { background: #eef2f7; }
  blockquote { border-left: 4px solid #2b6cb0; margin: 0.8em 0; padding: 2px 12px; background: #f5f8fc; }
  hr { border: none; border-top: 1px solid #d7dce2; margin: 1.4em 0; }
  a { color: #2b6cb0; }
</style></head><body>
${out.join('\n')}
</body></html>`;

writeFileSync(outFile, html, 'utf8');
console.log(`wrote ${outFile} (${out.length} blocks)`);
