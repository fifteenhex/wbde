# fuzz.py SEED N: N random commands for wbde --script.  Run them on a host
# build with -Db_sanitize=address,undefined and WBDE_NOQUIT=1, which makes
# Workbench > Quit do nothing.
import random, sys
r = random.Random(int(sys.argv[1]))
out = []
for i in range(int(sys.argv[2])):
    k = r.random()
    x, y = r.randrange(640), r.randrange(240)
    if k < 0.25: out.append(f"click {x} {y}")
    elif k < 0.40: out.append(f"dclick {x} {y}")
    elif k < 0.55:
        out.append(f"move {x} {y}"); out.append("down 1")
        out.append(f"move {r.randrange(640)} {r.randrange(240)}"); out.append("up 1")
    elif k < 0.75: out.append(f"menu {x} {y} {r.randrange(250)} {r.randrange(5,12)} hold"); out.append(f"move {r.randrange(300)} {r.randrange(10,140)}"); out.append("up 2")
    elif k < 0.85: out.append(f"key {r.choice([1,14,28,103,108,105,106,30,44])} {r.choice([0,0,128,1])}")
    elif k < 0.92: out.append("type ls|")
    else: out.append(f"wait {r.randrange(10,300)}")
out.append("shot /tmp/wbde-work/fuzz.ppm")
print("\n".join(l for l in out if 'quit' not in l))
