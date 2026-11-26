import json
import os
import sys
import time
import urllib.request

REPO = "zhengyuechushi/quark-dl-native"
PROXY = "http://127.0.0.1:7890"
TOKEN = os.environ["GIT_TOKEN"]
OPENER = urllib.request.build_opener(
    urllib.request.ProxyHandler({"http": PROXY, "https": PROXY})
)


def api(path):
    req = urllib.request.Request(
        "https://api.github.com/repos/%s/%s" % (REPO, path),
        headers={"Authorization": "token " + TOKEN, "Accept": "application/vnd.github+json"},
    )
    for attempt in range(4):
        try:
            with OPENER.open(req, timeout=40) as resp:
                return json.load(resp)
        except Exception as exc:
            if attempt == 3:
                raise
            print("retry:", exc, flush=True)
            time.sleep(5)


runs = api("actions/runs?per_page=1")["workflow_runs"]
run = runs[0]
print("run", run["id"], run["head_sha"][:7], run["status"], run["conclusion"], flush=True)

deadline = time.time() + int(sys.argv[1] if len(sys.argv) > 1 else 900)
while time.time() < deadline:
    run = api("actions/runs/%d" % run["id"])
    print(time.strftime("%H:%M:%S"), run["status"], run["conclusion"], flush=True)
    if run["status"] == "completed":
        break
    time.sleep(30)

print("FINAL", run["status"], run["conclusion"], flush=True)
if run["status"] == "completed":
    for job in api("actions/runs/%d/jobs" % run["id"])["jobs"]:
        print("job", job["name"], job["conclusion"], job["id"], flush=True)
        for step in job["steps"]:
            if step["conclusion"] not in ("success", "skipped"):
                print("   !!", step["name"], step["conclusion"], flush=True)
