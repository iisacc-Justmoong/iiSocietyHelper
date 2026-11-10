"""Independent publishers/consumers and selected-container following."""
import json
import os
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time

exe, build, version = sys.argv[1:]
assert subprocess.check_output([exe, '--version'], text=True).strip().endswith(version)
fixture = tempfile.TemporaryDirectory(prefix='process-fixture-', dir=build)
base = pathlib.Path(fixture.name)
roots = [base / (name + '.societycontainer') for name in ('first', 'second')]
ids = ['11111111-2222-4333-8444-555555555555', 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee']
sections = ['Files', 'Photos', 'Asset Library', 'Generation History', 'Models', 'Thinking Space', 'Forked', 'Published', 'Deleted']
for root, identifier in zip(roots, ids):
    root.mkdir(exist_ok=True)
    for name in sections:
        (root / name).mkdir(exist_ok=True)
    (root / '.society-drive.json').write_text(json.dumps(dict(type='SocietyDrive', schemaVersion=1, identifier=identifier)))
selection = base / 'storage.json'
def select(index):
    temporary = selection.with_suffix('.tmp')
    temporary.write_text(json.dumps(dict(schemaVersion=1, path=str(roots[index]), containerId=ids[index])))
    temporary.replace(selection)
def stream(process):
    values = queue.Queue()
    def collect():
        for line in process.stdout:
            values.put(json.loads(line))
    threading.Thread(target=collect, daemon=True).start()
    return values
def until(values, predicate):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        value = values.get(timeout=max(.1, deadline-time.monotonic()))
        if predicate(value):
            return value
    raise AssertionError('State was not delivered')
children = []
try:
    for mode in (['--index-list', ''], ['--index-get', ''], ['--scan-interval-ms', '0'],
                 ['--once', '--index-list', 'Files'], ['--read-state', '--watch']):
        assert subprocess.run([exe, *mode], capture_output=True).returncode == 2
    # A one-shot producer must complete its index before it exits.
    subprocess.run([exe, '--root', str(roots[0]), '--identifier', ids[0], '--once'],
                   capture_output=True, text=True, check=True, timeout=75)
    listed = subprocess.run([exe, '--root', str(roots[0]), '--identifier', ids[0], '--index-list', 'Files'],
                            capture_output=True, text=True, check=True, timeout=20)
    assert json.loads(listed.stdout) == []
    # Heartbeats must advance independently while the next full scan is deferred.
    paced_bound = ['--exit-after-ms', '12000'] if os.name == 'nt' else []
    paced = subprocess.Popen([exe, '--root', str(roots[0]), '--identifier', ids[0],
                              '--interval-ms', '100', '--scan-interval-ms', '60000', *paced_bound],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    children.append(paced)
    first = until(stream(paced), lambda v: v['daemonRunning'])
    paced_file = roots[0] / 'Files/paced.txt'
    paced_file.write_text('awaiting next scan')
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        observed = subprocess.run([exe, '--root', str(roots[0]), '--identifier', ids[0], '--read-state'],
                                  capture_output=True, text=True, check=True, timeout=20)
        heartbeat = json.loads(observed.stdout)
        if int(heartbeat['observedAtMs']) > int(first['observedAtMs']):
            break
        time.sleep(.1)
    assert int(heartbeat['observedAtMs']) > int(first['observedAtMs'])
    assert heartbeat['revision'] == first['revision']
    assert next(s for s in heartbeat['sections'] if s['key'] == 'files')['files'] == 0
    if os.name != 'nt':
        paced.terminate()
    assert paced.wait(timeout=15) == 0
    paced_file.unlink()
    # Service remains running before the first selected drive is available.
    selection.unlink(missing_ok=True)
    # Windows terminate() calls TerminateProcess, which cannot run shutdown
    # handlers. The existing bounded service run exercises a graceful exit there.
    bounded = ['--exit-after-ms', '12000'] if os.name == 'nt' else []
    daemon = subprocess.Popen([exe, '--selection-file', str(selection), '--interval-ms', '100',
                               '--scan-interval-ms', '100', *bounded], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    children.append(daemon)
    published = stream(daemon)
    time.sleep(.2)
    assert daemon.poll() is None
    select(0)
    initial = until(published, lambda v: v['containerId'] == ids[0] and v['daemonRunning'])
    subscribers = []
    for _ in ('com.iisacc.society', 'com.iisacc.dreamscapes'):
        process = subprocess.Popen([exe, '--root', str(roots[0]), '--identifier', ids[0], '--watch', '--interval-ms', '50'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        children.append(process)
        received = stream(process)
        assert until(received, lambda v: v['daemonRunning'])['revision'] == initial['revision']
        subscribers.append(received)
    (roots[0] / 'Files/shared.txt').write_text('native state delivery')
    for received in subscribers:
        updated = until(received, lambda v: 'files' in v['changedSections'] and int(v['revision']) > int(initial['revision']))
        assert next(s for s in updated['sections'] if s['key'] == 'files')['files'] == 1
    deadline = time.monotonic() + 15
    indexed = []
    while time.monotonic() < deadline:
        result = subprocess.run([exe, '--root', str(roots[0]), '--identifier', ids[0], '--index-list', 'Files'],
                                capture_output=True, text=True)
        if result.returncode == 0:
            indexed = json.loads(result.stdout)
            if any(item['name'] == 'shared.txt' for item in indexed):
                break
        time.sleep(.1)
    item = next(item for item in indexed if item['name'] == 'shared.txt')
    assert len(item['key']) == 64 and len(item['metadataRevision']) == 64
    found = subprocess.run([exe, '--root', str(roots[0]), '--identifier', ids[0], '--index-get', item['key']],
                           capture_output=True, text=True, check=True)
    assert json.loads(found.stdout)['path'] == 'Files/shared.txt'
    select(1)
    until(published, lambda v: v['containerId'] == ids[1] and v['daemonRunning'])
    for received in subscribers:
        until(received, lambda v: not v['daemonRunning'])
    # Consumers cannot accidentally accept another drive's UUID.
    wrong = subprocess.run([exe, '--root', str(roots[1]), '--identifier', ids[0], '--read-state'], capture_output=True)
    assert wrong.returncode == 1
    if os.name != 'nt':
        daemon.terminate()
    assert daemon.wait(timeout=15) == 0
    final = subprocess.run([exe, '--root', str(roots[1]), '--read-state'], capture_output=True, text=True, check=True)
    assert json.loads(final.stdout)['daemonRunning'] is False
    restarted = subprocess.Popen([exe, '--root', str(roots[1]), '--interval-ms', '100'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    children.append(restarted)
    until(stream(restarted), lambda v: v['daemonRunning'])
    observer = subprocess.Popen([exe, '--root', str(roots[1]), '--watch', '--interval-ms', '50'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    children.append(observer)
    observed = stream(observer)
    until(observed, lambda v: v['daemonRunning'])
    # Abrupt termination cannot publish offline; the subscription must detect
    # expiry from the heartbeat without another producer writing a snapshot.
    restarted.kill()
    restarted.wait(timeout=5)
    until(observed, lambda v: not v['daemonRunning'] and v['available'])
    assert (roots[0] / 'Files/shared.txt').read_text() == 'native state delivery'
    print('Daemon selection, two independent consumers, graceful shutdown and crash expiry passed')
finally:
    for process in children:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
    fixture.cleanup()
