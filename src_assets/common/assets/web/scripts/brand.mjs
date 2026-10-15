// Fork addition: the product is Scirocco, but the sources keep upstream's "Vibepollo" so merges
// stay clean. This rewrites the built web UIs (both of them) in place:
//
//   node brand.mjs <web output root> <name>
//
// Only the product name is replaced. Kept as they are: the drivers, which Windows itself shows
// under their own names ("Vibepollo Display Driver", "Vibepollo Virtual Gamepad"), the Playnite
// plugin, file names (VibepolloSetup.exe, Vibepollo.msi), URLs and identifiers. Running it twice
// changes nothing.
import { readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { extname, join } from 'node:path';

// A full stop ends a sentence ("... in Vibepollo."), but "Vibepollo.msi" is a file name; a
// lowercase suffix is grammar in some languages ("a Vibepollo-ban"), "Vibepollo-x86_64" a file.
const KEEP_AFTER = String.raw`(?![\w/\\]|-(?!\p{Ll}+(?![\w.\-]))|\.\w|\s+(?:[Vv]irtual|Display|Plug-?in|Playnite)\b)`;
const BRAND = new RegExp(String.raw`(?<![\w.\-/\\])Vibepollo` + KEEP_AFTER, 'gu');
const TEXT = new Set(['.html', '.js', '.json', '.css', '.txt', '.webmanifest']);

export function brand(text, name) {
  return text.replace(BRAND, name);
}

function walk(dir, out = []) {
  for (const entry of readdirSync(dir)) {
    const path = join(dir, entry);
    if (statSync(path).isDirectory()) walk(path, out);
    else if (TEXT.has(extname(entry))) out.push(path);
  }
  return out;
}

const [root, name] = process.argv.slice(2);
if (root && name) {
  let changed = 0;
  for (const file of walk(root)) {
    const before = readFileSync(file, 'utf-8');
    const after = brand(before, name);
    if (after !== before) {
      writeFileSync(file, after);
      changed += 1;
    }
  }
  console.log(`brand: "${name}" in ${changed} files under ${root}`);
}
