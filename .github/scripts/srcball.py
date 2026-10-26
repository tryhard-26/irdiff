#!/usr/bin/env python3
# usage: srcball.py VERSION OUT.tar.gz
import gzip
import io
import subprocess
import sys
import tarfile

version, out = sys.argv[1], sys.argv[2]
prefix = f"irdiff-{version}/"
entries = subprocess.run(["git", "ls-files", "-s", "-z"], check=True,
                         capture_output=True).stdout.split(b"\0")
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
    for e in filter(None, entries):
        meta, path = e.split(b"\t", 1)
        mode, blob = meta.split()[:2]
        path = path.decode()
        if path.startswith("Formula/"):
            continue
        data = subprocess.run(["git", "cat-file", "blob", blob.decode()],
                              check=True, capture_output=True).stdout
        info = tarfile.TarInfo(prefix + path)
        info.size = len(data)
        info.mode = 0o755 if mode == b"100755" else 0o644
        tar.addfile(info, io.BytesIO(data))
with open(out, "wb") as f, gzip.GzipFile(fileobj=f, mode="wb", mtime=0,
                                         compresslevel=9, filename="") as gz:
    gz.write(buf.getvalue())
