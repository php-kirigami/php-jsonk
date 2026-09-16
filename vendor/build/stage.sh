#!/bin/bash
# Downloads simdjson + yyjson (both ship as a handful of plain .c/.h files --
# no CMake/build step needed, unlike php-mdhtml's cmark-gfm) at the versions
# pinned in matrix.json (the last entry of each library's "versions" array --
# same convention as php-wasm-compiler, see its CLAUDE.md decision 28), and
# stages them under vendor/<lib>/. Re-run any time matrix.json changes.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
ROOT="$(pwd)"

for lib in simdjson yyjson; do
	version=$(node -e "
		const m = require('./matrix.json');
		const lib = m.libraries['$lib'];
		process.stdout.write(lib.tagPrefix + lib.versions[lib.versions.length - 1]);
	")
	repo=$(node -e "process.stdout.write(require('./matrix.json').libraries['$lib'].repo)")

	dest="$ROOT/vendor/$lib"
	mkdir -p "$dest"
	echo "--- staging $lib @ $version ($repo) ---"

	while IFS= read -r filename; do
		url_template=$(node -e "
			const m = require('./matrix.json');
			process.stdout.write(m.libraries['$lib'].assets['$filename']);
		")
		url="${url_template//\{repo\}/$repo}"
		url="${url//\{tag\}/$version}"
		echo "  $filename <- $url"
		curl -fsSL "$url" -o "$dest/$filename"
	done < <(node -e "
		const m = require('./matrix.json');
		for (const name of Object.keys(m.libraries['$lib'].assets)) console.log(name);
	")
done

echo "--- staged ---"
find "$ROOT/vendor/simdjson" "$ROOT/vendor/yyjson" -type f
