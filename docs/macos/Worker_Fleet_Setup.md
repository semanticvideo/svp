# Worker Fleet Setup on macOS

How to set up Macs as SVP workers for distributed builds, add new Macs to the
fleet, and keep every worker on the newest SVP build.

A worker is installed once. That install asks for the Mac's administrator
password one time. After that, pairing and every SVP update reach the worker
with no password, no SSH key, and no manual step.

## Requirements

- Apple Silicon Mac, on the same macOS version as the coordinators.
- An SVP install folder (`bin/`, `libexec/`, `share/`, about 130 MB) built with
  the runtime bundle (`cmake --install` with `SVP_RUNTIME_BUNDLE_DIR` set).
  Copy the whole folder, not only `bin/svp-builder`.
- Remote Login turned on, if you copy the install and run the install command
  over SSH. SVP itself does not use SSH for fleet workers.

Any number of Macs works, including one Mac with no workers.

## Terms

- **Coordinator:** a Mac that runs builds and sends work to workers.
- **Worker:** a Mac whose worker service runs that work. A Mac can be both.
- **Fleet:** the set of coordinators and workers that share one fleet secret.
- **Worker token** (`svpjoin1.…`): lets a new Mac join the fleet as a worker.
  It cannot pair other Macs or act as a coordinator. It admits new Macs for a
  limited time (7 days by default, at most 90).
- **Coordinator token** (`svpfleet1.…`): the whole fleet secret. Give it only
  to Macs that should send work. It does not expire.

## 1. Create the fleet (once)

On the first coordinator:

```bash
svp-builder workers fleet init
```

The fleet secret is stored in
`~/Library/Application Support/SVP/Fleet/fleet.json` (mode 0600).

## 2. Add more coordinators (optional)

On the first coordinator, print a coordinator token and pass it to each other
coordinator through stdin, so it never appears on a command line:

```bash
svp-builder workers fleet token --coordinator | ssh <user>@<coordinator> 'svp-builder workers fleet join -'
```

## 3. Install a new worker

1. On a coordinator, save a worker token to a file only you can read:

   ```bash
   umask 077; svp-builder workers fleet token > svp-worker-token.txt
   ```

   Use `--valid-days <n>` to admit new Macs for longer than 7 days.

2. Copy the SVP install folder and the token file to the new Mac.

3. On the new Mac, run the install from that folder and type the
   administrator password once when asked:

   ```bash
   <install>/bin/svp-builder worker install --join - < svp-worker-token.txt
   ```

   This installs the worker service (LaunchDaemon `org.svp.worker`) running
   the runtime of that install folder, stores the join credential (0600), and
   starts advertising the Mac as joinable. Delete the token file afterwards.

   `--dry-run` stages everything and prints the sudo command without running
   it.

## 4. Pairing (automatic)

Every coordinator of the fleet pairs joinable workers on its own, before each
`build --distributed`. To pair right away and push this Mac's runtime and
models:

```bash
svp-builder workers fleet pair
```

At its first pairing a worker receives a long-term fleet member key, so
coordinators that join the fleet later can still pair it after its worker
token has expired. Check the fleet with:

```bash
svp-builder workers list
```

## 5. Updates (automatic)

Install the new SVP build on the coordinator you build from. The next time it
talks to a worker (`build --distributed`, `build-batch`, `interlace
create-batch`, `workers sync`, or `workers fleet pair`), it pushes its runtime.
The worker checks every file against the runtime's BLAKE3 manifest,
test-starts it, waits until no job is running, switches to it, and restarts
itself. Coordinators wait for a restarting worker instead of dropping it.

To update every worker at once, run `svp-builder workers sync <worker>` for
each paired worker, or `svp-builder workers fleet pair`.

Rules:

- A worker never moves to an older build. The newest build wins, by the
  release stamp the install writes (`libexec/svp/runtime/release.json`).
- If the new runtime fails its test-start, the worker stays on its current
  runtime and logs why.
- Jobs always run on the runtime of the coordinator that sent them.
- Old runtimes are not deleted.

## How the worker service is laid out

- Worker root: `/Library/Application Support/SVP/Worker`, owned by the worker
  user.
- `<worker root>/current` links to `runtimes/<runtime-id>`. The LaunchDaemon
  runs `<worker root>/current/bin/svp-builder worker serve --root <worker
  root>`. The worker repoints this link itself when it updates, which is why
  updates need no password.
- Log: `<worker root>/logs/agent.log`.
- Pairings: `<worker root>/pairings/`. Pairings added while the worker runs
  are served immediately, on the same port.

## Macs paired over SSH

`svp-builder workers pair <user>@<host> [--system-service]` still works and
installs the same self-updating layout. A worker whose LaunchDaemon still
names a fixed `runtimes/<id>/bin/svp-builder` path does not update itself; it
must be moved once to the `current` layout (repoint the plist to
`<worker root>/current/bin/svp-builder`, which needs sudo on that Mac).

## Troubleshooting

- **A worker is not found:** check that its service runs (`pgrep -fl "worker
  serve"`) and read `<worker root>/logs/agent.log`. A failed Bonjour
  advertisement is retried in the background and never stops the service.
- **A new Mac never pairs:** its worker token may have expired. Print a new
  token and run `worker install --join` again on that Mac.
- **A worker did not update:** the log names the reason, for example a failed
  test-start. Workers only switch when no job is running.
- **Remove a fleet worker from a coordinator:** fleet workers have no SSH
  target, so use `svp-builder workers unpair --forget <pairing-id>`.
