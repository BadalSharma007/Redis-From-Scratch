# learning/ch11/test_cmds.py
#
# Command-level test, in the style of the book's test_cmds.py: it runs
# the real ./kv_client against a RUNNING ./kv_server and checks every
# output line.
#
# How to use:
#   terminal 1:  ./kv_server
#   terminal 2:  python3 test_cmds.py
#
# It is safe to run more than once: it deletes its own keys first.

import shlex
import subprocess

CASES = r'''
$ ./kv_client zscore asdf n1
(nil)
$ ./kv_client zquery xxx 1 asdf 1 10
(arr) len=0
(arr) end
$ ./kv_client zadd zset 1 n1
(int) 1
$ ./kv_client zadd zset 2 n2
(int) 1
$ ./kv_client zadd zset 1.1 n1
(int) 0
$ ./kv_client zscore zset n1
(dbl) 1.1
$ ./kv_client zquery zset 1 "" 0 10
(arr) len=4
(str) n1
(dbl) 1.1
(str) n2
(dbl) 2
(arr) end
$ ./kv_client zquery zset 1.1 "" 1 10
(arr) len=2
(str) n2
(dbl) 2
(arr) end
$ ./kv_client zquery zset 1.1 "" 2 10
(arr) len=0
(arr) end
$ ./kv_client zrem zset adsf
(int) 0
$ ./kv_client zrem zset n1
(int) 1
$ ./kv_client zquery zset 1 "" 0 10
(arr) len=2
(str) n2
(dbl) 2
(arr) end
$ ./kv_client zadd zset 3 n3
(int) 1
$ ./kv_client zadd zset 4 n4
(int) 1
$ ./kv_client zquery zset 0 "" 0 2
(arr) len=2
(str) n2
(dbl) 2
(arr) end
$ ./kv_client zquery zset 0 "" 0 4
(arr) len=4
(str) n2
(dbl) 2
(str) n3
(dbl) 3
(arr) end
$ ./kv_client zquery zset 4 "" -1 10
(arr) len=4
(str) n3
(dbl) 3
(str) n4
(dbl) 4
(arr) end
$ ./kv_client zquery zset 0 "" 0 0
(arr) len=0
(arr) end
$ ./kv_client zadd zset 2 aaa
(int) 1
$ ./kv_client zquery zset 2 "" 0 4
(arr) len=4
(str) aaa
(dbl) 2
(str) n2
(dbl) 2
(arr) end
$ ./kv_client set plain v
(nil)
$ ./kv_client get plain
(str) v
$ ./kv_client zadd plain 1 a
(err) 3 expect zset
$ ./kv_client zscore plain a
(err) 3 expect zset
$ ./kv_client get zset
(err) 3 expect string type
$ ./kv_client set zset x
(err) 3 a non-string value exists
$ ./kv_client zadd zset abc n9
(err) 4 expect fp number
$ ./kv_client zquery zset 1 "" x 10
(err) 4 expect int
$ ./kv_client zquery zset 1 "" 0 y
(err) 4 expect int
$ ./kv_client del zset
(int) 1
$ ./kv_client zscore zset n2
(nil)
$ ./kv_client del plain
(int) 1
$ ./kv_client zadd z2 5 only
(int) 1
$ ./kv_client keys
(arr) len=1
(str) z2
(arr) end
$ ./kv_client del z2
(int) 1
'''

# --- start from a clean slate (output ignored) ---
for key in ["zset", "plain", "z2", "asdf", "xxx"]:
    subprocess.run(["./kv_client", "del", key], capture_output=True)

# --- split the text into (command, expected output) pairs ---
cmds = []
outputs = []
for x in CASES.splitlines():
    x = x.strip()
    if not x:
        continue
    if x.startswith('$ '):
        cmds.append(x[2:])
        outputs.append('')
    else:
        outputs[-1] = outputs[-1] + x + '\n'

assert len(cmds) == len(outputs)

# --- run every command and compare ---
for cmd, expect in zip(cmds, outputs):
    out = subprocess.check_output(shlex.split(cmd)).decode('utf-8')
    assert out == expect, f'cmd: {cmd}\n got: {out!r}\n want: {expect!r}'

print(f'all {len(cmds)} command cases passed')
