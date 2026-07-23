#!/usr/bin/env python3
"""Build-and-run smoke tests for the Corosio example programs.

Each example is a real, buildable program; this drives the runnable ones
end-to-end against a Python peer (or as a standalone), asserting on output.
https-client is exercised only for build + usage, since a successful TLS
session needs a peer whose certificate chains to a trusted root.

Assumes the examples are already built under build/example/*/Release/.
Exit code is non-zero if any runnable example fails.
"""
import socket, subprocess, sys, os, time, threading, contextlib

ROOT  = '/home/sgerbino/workspace/boost/libs/corosio'
BUILD = ROOT + '/build/example'
BIN = {
    'echo':      BUILD + '/echo-server/Release/corosio_example_echo_server',
    'hash':      BUILD + '/hash-server/Release/corosio_example_hash_server',
    'http':      BUILD + '/client/Release/corosio_example_client_http',
    'https':     BUILD + '/https-client/Release/corosio_example_client_https',
    'nslookup':  BUILD + '/nslookup/Release/corosio_example_nslookup',
    'reconnect': BUILD + '/reconnect/Release/corosio_example_reconnect',
}
results = []
def record(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f"  — {detail}" if detail else ''))

def free_port():
    s = socket.socket(); s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close(); return p

def wait_port(port, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        with contextlib.closing(socket.socket()) as s:
            s.settimeout(0.3)
            if s.connect_ex(('127.0.0.1', port)) == 0: return True
        time.sleep(0.05)
    return False

def one_shot_server(port, handler, ready):
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', port)); srv.listen(1); srv.settimeout(8); ready.set()
    try:
        conn, _ = srv.accept()
        with conn: handler(conn)
    except Exception:
        pass
    finally:
        srv.close()

# ---- nslookup: standalone, resolve localhost --------------------------------
def test_nslookup():
    try:
        p = subprocess.run([BIN['nslookup'], 'localhost'], capture_output=True,
                           text=True, timeout=15)
    except subprocess.TimeoutExpired:
        return record('nslookup localhost', False, 'timeout')
    out = p.stdout + p.stderr
    ok = p.returncode == 0 and ('127.0.0.1' in out or '::1' in out)
    record('nslookup localhost', ok, (out.strip().replace('\n',' ')[:60]) if ok else f'rc={p.returncode} out={out[:80]!r}')

# ---- echo-server: round-trip ------------------------------------------------
def test_echo():
    port = free_port()
    srv = subprocess.Popen([BIN['echo'], str(port), '1'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_port(port): return record('echo-server round-trip', False, 'server did not listen')
        payload = b'hello corosio echo'
        with contextlib.closing(socket.create_connection(('127.0.0.1', port), timeout=5)) as c:
            c.sendall(payload)
            got = b''
            c.settimeout(5)
            while len(got) < len(payload):
                chunk = c.recv(4096)
                if not chunk: break
                got += chunk
        record('echo-server round-trip', got == payload, f'sent {len(payload)}B, echoed {len(got)}B')
    finally:
        srv.terminate();  srv.wait(timeout=5)

# ---- hash-server: send data, expect a hex digest line -----------------------
def test_hash():
    port = free_port()
    srv = subprocess.Popen([BIN['hash'], str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_port(port): return record('hash-server digest', False, 'server did not listen')
        with contextlib.closing(socket.create_connection(('127.0.0.1', port), timeout=5)) as c:
            c.sendall(b'the quick brown fox')
            c.settimeout(5)
            line = b''
            while not line.endswith(b'\n'):
                chunk = c.recv(256)
                if not chunk: break
                line += chunk
        txt = line.decode(errors='replace').strip()
        ok = len(txt) >= 8 and all(ch in '0123456789abcdefABCDEF' for ch in txt)
        record('hash-server digest', ok, f'digest={txt[:32]}')
    finally:
        srv.terminate(); srv.wait(timeout=5)

# ---- http_client vs a one-shot HTTP responder -------------------------------
def test_http():
    port = free_port(); ready = threading.Event()
    def handler(conn):
        conn.settimeout(5)
        try: conn.recv(4096)            # read the request
        except Exception: pass
        conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 13\r\n'
                     b'Connection: close\r\n\r\nhello, client')
    t = threading.Thread(target=one_shot_server, args=(port, handler, ready)); t.start()
    ready.wait(3)
    try:
        p = subprocess.run([BIN['http'], '127.0.0.1', str(port)],
                           capture_output=True, text=True, timeout=15)
        ok = p.returncode == 0 and 'hello, client' in p.stdout
        record('http_client GET', ok, f"rc={p.returncode}")
    except subprocess.TimeoutExpired:
        record('http_client GET', False, 'timeout')
    finally:
        t.join(timeout=5)

# ---- reconnect: connects, runs a session, then re-enters the retry loop ------
def test_reconnect():
    port = free_port(); ready = threading.Event()
    def handler(conn):
        time.sleep(0.2)   # hold the session briefly, then close to trigger reconnect
    t = threading.Thread(target=one_shot_server, args=(port, handler, ready)); t.start()
    ready.wait(3)
    proc = subprocess.Popen([BIN['reconnect'], '127.0.0.1', str(port)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = ''
    end = time.time() + 4
    try:
        while time.time() < end:
            if proc.poll() is not None: break
            time.sleep(0.1)
    finally:
        proc.terminate()
        try: out = proc.communicate(timeout=5)[0]
        except subprocess.TimeoutExpired: proc.kill(); out = proc.communicate()[0]
        t.join(timeout=5)
    record('reconnect connects', 'Connected on attempt 1' in (out or ''),
           (out or '').strip().replace('\n', ' | ')[:70])

# ---- https-client: build + usage only ---------------------------------------
def test_https_usage():
    try:
        p = subprocess.run([BIN['https']], capture_output=True, text=True, timeout=10)
        ok = p.returncode != 0 and ('sage' in (p.stdout + p.stderr))  # prints usage
        record('https_client (build+usage)', ok, 'runtime needs a trusted TLS peer; build+usage verified')
    except Exception as e:
        record('https_client (build+usage)', False, str(e))

def main():
    missing = [k for k, v in BIN.items() if not os.path.exists(v)]
    if missing:
        print('Missing built binaries (build the examples first):', missing); sys.exit(2)
    test_nslookup(); test_echo(); test_hash(); test_http(); test_reconnect(); test_https_usage()
    n_fail = sum(1 for _, ok, _ in results if not ok)
    print(f"\n{len(results)-n_fail}/{len(results)} passed")
    sys.exit(1 if n_fail else 0)

if __name__ == '__main__':
    main()
