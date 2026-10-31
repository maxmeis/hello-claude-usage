import { readFile } from 'node:fs/promises';
import { execFileSync, spawnSync } from 'node:child_process';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const site = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const link = JSON.parse(await readFile(resolve(site, '.vercel/project.json'), 'utf8'));
const cli = ['dlx', '--allow-build', 'esbuild', '--allow-build', 'koffi', 'vercel'];
const common = { cwd: site, encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 };
const endpoint = `/v9/projects/${link.projectId}`;
const api = (...args) => [...cli, 'api', endpoint, '--scope', link.orgId, ...args];
const settings = JSON.parse(execFileSync('pnpm', api('--raw'), common));
const rootDirectory = settings.rootDirectory ?? null;

function setRoot(value) {
  execFileSync('pnpm', api('-X', 'PATCH', '--input', '-', '--silent'), {
    ...common, input: JSON.stringify({ rootDirectory: value }),
  });
}

// Git deployments use web-installer/ inside the firmware repository. CLI
// uploads are already scoped to that folder, so do not apply the prefix twice.
let exitCode = 1;
try {
  if (rootDirectory !== null) setRoot(null);
  const result = spawnSync('pnpm', [...cli, 'deploy', '--prod', '--yes', '--scope', link.orgId], {
    cwd: site, stdio: 'inherit', timeout: 10 * 60 * 1000,
  });
  exitCode = result.status ?? 1;
} finally {
  if (rootDirectory !== null) setRoot(rootDirectory);
}
process.exitCode = exitCode;
