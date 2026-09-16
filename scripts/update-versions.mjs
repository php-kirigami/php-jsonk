#!/usr/bin/env node
/**
 * Refreshes matrix.json's "versions" arrays by querying the GitHub API --
 * same approach as php-wasm-compiler's compile/update-lib-versions.mjs
 * (see that repo's CLAUDE.md decisions 11/28/33: matrix.json is the single
 * source of truth for a dependency's version, "versions" is an honest
 * mirror of upstream with the last entry being what actually gets built,
 * never a hand-maintained duplicate elsewhere).
 *
 * Usage: node scripts/update-versions.mjs [--write]
 *   --write   Persist changes to matrix.json. Without it, just prints a
 *             report of what would change (dry run).
 */
import { readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const sourceDir = path.dirname(fileURLToPath(import.meta.url));
const matrixPath = path.resolve(sourceDir, '..', 'matrix.json');

async function fetchJSON(url) {
	const response = await fetch(url, {
		headers: { 'User-Agent': 'php-jsonk' },
	});
	if (!response.ok) {
		throw new Error(`HTTP ${response.status} for ${url}`);
	}
	return response.json();
}

function normalizeVersion(tag) {
	const match = tag.replace(/_/g, '.').match(/(\d+(?:\.\d+){1,3}(?:-\d+)?)/);
	return match ? match[1] : tag;
}

async function latestGithubRelease(repo) {
	try {
		const release = await fetchJSON(
			`https://api.github.com/repos/${repo}/releases/latest`
		);
		return normalizeVersion(release.tag_name);
	} catch {
		return null;
	}
}

async function latestGithubTag(repo) {
	const tags = await fetchJSON(
		`https://api.github.com/repos/${repo}/tags?per_page=100`
	);
	const versions = tags
		.map((t) => t.name)
		.filter((name) => /^v?\d+(\.\d+){1,3}(-\d+)?$/.test(name))
		.map(normalizeVersion);
	if (versions.length === 0) {
		return null;
	}
	const toParts = (v) => v.replace('-', '.').split('.').map(Number);
	versions.sort((a, b) => {
		const pa = toParts(a);
		const pb = toParts(b);
		for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
			const diff = (pa[i] || 0) - (pb[i] || 0);
			if (diff !== 0) return diff;
		}
		return 0;
	});
	return versions[versions.length - 1];
}

export async function updateVersions({ write = false } = {}) {
	const matrix = JSON.parse(readFileSync(matrixPath, 'utf8'));
	let changed = 0;

	for (const [name, lib] of Object.entries(matrix.libraries)) {
		if (lib.sourceType !== 'github-release' && lib.sourceType !== 'github-tag') {
			continue;
		}
		const repo = lib.repo;
		const latest =
			lib.sourceType === 'github-release'
				? (await latestGithubRelease(repo)) ?? (await latestGithubTag(repo))
				: await latestGithubTag(repo);

		if (!latest) {
			console.warn(`⚠️  ${name}: could not resolve a version from ${repo}`);
			continue;
		}

		const current = lib.versions[lib.versions.length - 1];
		if (latest !== current) {
			console.log(`${name}: ${current ?? '(none)'} → ${latest}`);
			if (!lib.versions.includes(latest)) {
				lib.versions.push(latest);
			}
			changed++;
		}
	}

	if (changed === 0) {
		console.log('✨ Both dependencies are already up to date!');
	} else if (write) {
		matrix.lastChecked = new Date().toISOString();
		writeFileSync(matrixPath, JSON.stringify(matrix, null, '\t') + '\n');
		console.log(`\nWrote ${changed} update(s) to ${matrixPath}`);
	} else {
		console.log(`\n${changed} update(s) found (dry run -- pass --write to persist).`);
	}

	return changed;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
	updateVersions({ write: process.argv.includes('--write') }).catch((error) => {
		console.error(error);
		process.exit(1);
	});
}
