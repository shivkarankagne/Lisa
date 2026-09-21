# W12 — the tests that need a person

Two of the 1.0 exit criteria cannot be scripted: they are about what
happens on a machine that has never seen LISA, with no network. Both are
run by hand and the results written into the W12 report.

Allow about 40 minutes for the two together.

---

## A. Clean-machine test

The point is that a stranger's Mac can run LISA with nothing installed:
no Homebrew, no Python, no Xcode, no model runner.

### Prepare (on your own account)

1. Build a release binary and package it:

       cd ~/lisa_assembly
       cmake --build build
       scripts/release.sh

   This writes `lisa-<version>-macos-arm64.tar.gz` and its `.sha256`.

2. Copy the archive and the two model files to a folder both accounts
   can read, for example `/Users/Shared/lisa-test/`:

       mkdir -p /Users/Shared/lisa-test
       cp build/lisa-*.tar.gz build/lisa-*.sha256 /Users/Shared/lisa-test/
       cp models/Qwen3-4B-Q4_K_M.gguf \
          models/Qwen3-Embedding-0.6B-Q8_0.gguf /Users/Shared/lisa-test/
       chmod -R a+rX /Users/Shared/lisa-test

3. Put 5 to 10 documents of your own in `/Users/Shared/lisa-test/docs/`
   (PDF, Word, text — include at least one scan).

### Create the account

System Settings → Users & Groups → Add User → Standard (not
Administrator). Name it `lisatest`. Log out, log in as `lisatest`.

**Do not install anything on this account.** That is the test.

### Run (as `lisatest`)

Record the wall-clock time from the first command to the first answer.

    mkdir ~/lisa && cd ~/lisa
    shasum -a 256 -c /Users/Shared/lisa-test/lisa-*.sha256
    tar xzf /Users/Shared/lisa-test/lisa-*.tar.gz
    ./lisa --version
    ./lisa model --set /Users/Shared/lisa-test/Qwen3-4B-Q4_K_M.gguf
    ./lisa model --set /Users/Shared/lisa-test/Qwen3-Embedding-0.6B-Q8_0.gguf
    ./lisa ingest --collection docs /Users/Shared/lisa-test/docs
    ./lisa ask --collection docs "<a question your documents answer>"

Then the GUI:

    ./lisa gui

In the window: accept or change the suggested folders, wait for
indexing, ask the same question, click a citation, open Settings.

### Record

| What | Value |
| :--- | :--- |
| macOS version on the account | |
| Checksum verified | yes / no |
| Gatekeeper prompt seen, and what got past it | (expected: right-click → Open, until the Developer ID is bought) |
| Anything it asked to install | (expected: nothing) |
| Time from `tar xzf` to first answer | |
| Ingest: files indexed, time | |
| Answer correct, with a citation | yes / no |
| Citation opened the right passage | yes / no |
| GUI: first-run panel, indexing, answer, Settings | |
| Anything confusing or broken | |

**Pass:** no installation of any kind, an answer with a working citation
from both the CLI and the GUI, and under five minutes from unpacking to
the first answer (plan §6 W11).

### Afterwards

Log back into your own account and delete the `lisatest` user and
`/Users/Shared/lisa-test` when you are done with both tests.

---

## B. Offline test

The point is that LISA never needs the network, and does not quietly try
to use it.

Run this **on the `lisatest` account as well**, before doing anything
else on it, so the two tests are one sitting.

1. Turn Wi-Fi **off** (menu bar → Wi-Fi → off). Unplug Ethernet if any.
   Turn off any phone tethering and hotspot.
2. Confirm there is no network:

       ping -c 2 8.8.8.8

   This must fail.
3. Run every step of section A from the copy onwards, with the network
   still off.
4. While LISA is running, in another Terminal tab, check that it holds
   no connection to anything but itself:

       lsof -nP -iTCP -a -c lisa

   Expected: only `127.0.0.1:<port>` in LISTEN, plus local connections
   from the GUI. Nothing to any outside address.
5. Turn Wi-Fi back on when finished.

### Record

| What | Value |
| :--- | :--- |
| `ping` failed as expected | yes / no |
| Everything in section A worked with no network | yes / no |
| `lsof` showed only 127.0.0.1 | yes / no |
| Any error mentioning the network | |

**Pass:** the whole of section A works with the network off, and `lsof`
shows no outside connection.

---

## Where the results go

Write both tables into the W12 report
(`LISA_REPORT_AND_UPDATE_015.md`, §7) and say which build they were run
against:

    git rev-parse --short HEAD
