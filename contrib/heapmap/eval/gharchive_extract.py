#!/usr/bin/env python3
"""Turn GH Archive hour files (https://data.gharchive.org/YYYY-MM-DD-H.json.gz)
into CSV rows "id,created_at,type,repo,actor" in file order, which is the
order the events were recorded in. Usage: gharchive_extract.py FILE... > out.csv"""
import csv, gzip, io, json, sys
from multiprocessing import Pool

def one(path):
    out = io.StringIO()
    w = csv.writer(out)
    with gzip.open(path, 'rt', encoding='utf-8') as f:
        for line in f:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            w.writerow([e['id'], e['created_at'], e['type'],
                        e.get('repo', {}).get('name', ''), e.get('actor', {}).get('login', '')])
    return out.getvalue()

if __name__ == '__main__':
    with Pool() as p:
        for chunk in p.imap(one, sys.argv[1:]):
            sys.stdout.write(chunk)
