import { execFileSync, spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';

// chronos-serve and chronos-e2e-fixture come from the repo's CMake build, ../build unless CHRONOS_BUILD names another.
const build = resolve(process.env.CHRONOS_BUILD ?? '../build');

// Writes the fixture days (ACME, 2019-12-27 and 2019-12-30) into a fresh temp dir and returns the dir.
export function fixture(): string {
  const dir = mkdtempSync(join(tmpdir(), 'chronos-fixture-'));
  execFileSync(join(build, 'chronos-e2e-fixture'), [dir]);
  return dir;
}

// Starts chronos-serve with `args` on a free port, serving web/dist from a temp working dir that holds its sessions/.
export async function serve(args: string[]): Promise<{ url: string; stop(): Promise<void> }> {
  const cwd = mkdtempSync(join(tmpdir(), 'chronos-serve-'));
  const child = spawn(join(build, 'chronos-serve'), [...args, '--port', '0', '--web', resolve('dist')], {
    cwd,
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  const exited = new Promise<number | null>((done) => child.once('exit', done));
  const clean = () => rmSync(cwd, { recursive: true, force: true });
  // Line 1: "chronos-serve: http://127.0.0.1:<port>/ with ..."
  const line = await Promise.race([
    new Promise<string>((done) => createInterface({ input: child.stdout! }).once('line', done)),
    exited.then((code): string => {
      clean();
      throw new Error(`chronos-serve exited ${code}`);
    }),
  ]);
  const url = /http:\/\/\S+?\//.exec(line)?.[0];
  if (!url) {
    child.kill();
    await exited;
    clean();
    throw new Error(`chronos-serve printed: ${line}`);
  }
  return {
    url,
    async stop() {
      child.kill('SIGTERM');
      const code = await exited;
      clean();
      if (code !== 0) throw new Error(`chronos-serve exited ${code}`);
    },
  };
}
