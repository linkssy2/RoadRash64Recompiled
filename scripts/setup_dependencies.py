"""Fetch pinned dependencies and apply this release's local changes. No ROM downloads."""
from pathlib import Path
import hashlib,json,subprocess,shutil,sys
ROOT=Path(__file__).resolve().parents[1]
def run(*args,cwd=ROOT,check=True):
    return subprocess.run(args,cwd=cwd,check=check,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
def main():
    records=json.loads((ROOT/'dependencies.lock.json').read_text())
    for entry in records:
        if entry.get('platforms') and sys.platform not in entry['platforms']:
            continue
        target=(ROOT/entry['path']).resolve()
        if ROOT not in target.parents:raise ValueError('Dependency path outside repository')
        if not (target/'.git').exists():
            if target.exists() and any(target.iterdir()):raise RuntimeError(f'Nonempty dependency folder: {target}')
            target.parent.mkdir(parents=True,exist_ok=True)
            run('git','clone','--no-checkout',entry['url'],str(target))
            # A pinned PR commit can remain fetchable on GitHub after a squash
            # merge without being included in a normal clone's branch history.
            present=run('git','cat-file','-e',entry['commit']+'^{commit}',cwd=target,check=False)
            if present.returncode!=0:
                run('git','fetch','origin',entry['commit'],cwd=target)
            run('git','checkout','--detach',entry['commit'],cwd=target)
        head=run('git','rev-parse','HEAD',cwd=target).stdout.decode().strip()
        if head!=entry['commit']:raise RuntimeError(f'Wrong dependency revision: {target}; preserve your changes and use a fresh source folder')
        if entry['patch']:
            patch=str(ROOT/entry['patch'])
            applied=run('git','apply','--reverse','--check',patch,cwd=target,check=False).returncode==0
            if not applied:
                run('git','apply','--check',patch,cwd=target)
                run('git','apply',patch,cwd=target)
        for extra in entry['extraFiles']:
            source=ROOT/'dependency-overrides'/entry['path']/extra['path']
            dest=target/extra['path']
            if hashlib.sha256(source.read_bytes()).hexdigest()!=extra['sha256']:raise RuntimeError(f'Override checksum mismatch: {source}')
            if dest.exists() and dest.read_bytes()!=source.read_bytes():raise RuntimeError(f'Preserving conflicting local file: {dest}')
            dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,dest)
        print(entry['path']+' ready at '+head[:12])
if __name__=='__main__':
    try:main()
    except subprocess.CalledProcessError as ex:
        raise SystemExit(ex.stderr.decode(errors='replace'))
