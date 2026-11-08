"""Validate the selected source, release tag and packaged artifact (stdlib only)."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent

def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True, encoding='utf-8').strip()

def validate():
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8'))
    name = os.environ['GITHUB_EVENT_NAME']
    if name not in ('push', 'workflow_dispatch'):
        raise ValueError('Only a new version tag or manual dispatch is allowed')
    manifest = json.loads((ROOT / 'plugin.json').read_text(encoding='utf-8-sig'))
    version = manifest['version']
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('Version must be X.Y.Z')
    variant, separator, prerelease, host = '', '', False, {}
    if (ROOT / 'release.json').exists():
        release = json.loads((ROOT / 'release.json').read_text(encoding='utf-8'))
        variant, separator = release['variant'], '-'
        prerelease, host = release['prerelease'], release.get('hostSource', {})
    elif (ROOT / 'edition.json').exists():
        variant = json.loads((ROOT / 'edition.json').read_text(encoding='utf-8'))['backend']
        separator = '/'
    if variant and variant not in ('gdi', 'qml'):
        raise ValueError('Unknown variant')
    tag = ''
    publish = name == 'push' or str(event.get('inputs', {}).get('publish', 'false')).lower() == 'true'
    if name == 'push':
        if not event.get('created') or event.get('deleted') or not event.get('ref', '').startswith('refs/tags/'):
            raise ValueError('Only newly created tags can publish automatically')
        tag = event['ref'].removeprefix('refs/tags/')
    elif publish:
        tag = event.get('inputs', {}).get('ref', '').removeprefix('refs/tags/')
    expected = f'{variant}{separator}v{version}'
    sha = git('rev-parse', 'HEAD')
    if publish:
        if tag != expected:
            raise ValueError(f'Publish requires existing version tag {expected}, got {tag!r}')
        if git('rev-parse', '--verify', f'refs/tags/{tag}^{{commit}}') != sha:
            raise ValueError('Checked out source does not match tag')
        subprocess.run(['git', 'merge-base', '--is-ancestor', sha, f'origin/{variant or "main"}'], cwd=ROOT, check=True)
    for name in ('CHANGELOG.md', 'CHANGELOG.en.md'):
        path = ROOT / name
        if (publish and name == 'CHANGELOG.md') or path.exists():
            text = path.read_text(encoding='utf-8')
            if not re.search(r'^## \[' + re.escape(version) + r'\][^\n]*\n\s*\S', text, re.M):
                raise ValueError(f'Missing nonempty {version} section in {name}')
    title = git('log', '-1', '--format=%s')
    if not re.fullmatch(r'(feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert)(\([a-zA-Z0-9_.-]+\))?!?: [\x20-\x7e]+', title):
        raise ValueError('Release source commit must use an English Angular subject')
    qt = manifest.get('runtime', {}).get('qtVersion', '')
    if variant and (ROOT / 'edition.json').exists():
        qt = json.loads((ROOT / 'edition.json').read_text(encoding='utf-8')).get('qtVersion', '')
    values = dict(sha=sha, tag=tag, publish=str(publish).lower(), version=version,
                  variant=variant, qt=qt, prerelease=str(prerelease).lower(),
                  host_repository=host.get('repository', ''), host_ref=host.get('ref', ''))
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
        for key, value in values.items():
            output.write(f'{key}={value}\n')
    print(f'Validated {manifest["id"]} {version} at {sha}; publish={publish}; tag={tag or "none"}')

def verify_package(folder):
    files = list(Path(folder).glob('*.wsplugin'))
    if len(files) != 1:
        raise ValueError('Expected exactly one package')
    package = files[0]
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    if package.with_suffix('.wsplugin.sha256').read_text(encoding='ascii').split()[0] != digest:
        raise ValueError('Package checksum mismatch')
    expected = json.loads((ROOT / 'plugin.json').read_text(encoding='utf-8-sig'))
    with zipfile.ZipFile(package) as archive:
        actual = json.loads(archive.read('plugin.json').decode('utf-8-sig'))
        if actual != expected:
            raise ValueError('Packaged manifest differs from source')
        required = [actual['runtime']['program']]
        if actual['runtime'].get('qt') == 'host':
            required += {'ra9fael.qr': ['QrApp.dll'], 'ra9fael.bosskey': ['BossKeyApp.dll'],
                         'ra9fael.binarylab': ['BinaryLabApp.dll']}.get(actual['id'], [])
        if actual.get('icon'):
            required.append(actual['icon'])
        for name in required:
            if not archive.read(name):
                raise ValueError(f'Empty runtime asset {name}')
    print(f'Verified {package.name}: SHA256 {digest}')

if __name__ == '__main__':
    if len(sys.argv) > 1:
        verify_package(sys.argv[1])
    else:
        validate()
