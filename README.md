# LISA

**Ask your own documents questions, on your own computer.**

LISA is one file. Point it at a folder of documents, ask a question in
plain language, and get an answer with citations to the exact passages
it came from. No Python, no Docker, no database server, no account, no
internet.

    $ lisa ask --collection manuals "How often must the pump bearings be inspected?"
    Every 500 hours [1].

    Sources:
      [1] /Users/you/Documents/manuals/maintenance.pdf, page 12

Everything happens on your machine: your documents, the search index,
and the model. LISA makes no network connections except its own server
on `127.0.0.1`, and sends nothing anywhere.

Status: **0.6.0, pre-release.** Apple Silicon Macs (macOS 13+). The work
left before 1.0 is in `LISA_COMPLETION_PLAN.md`.

---

## Quickstart (about five minutes, most of it downloading a model)

**1. Get `lisa`.** Download the binary for Apple Silicon from
[Releases](https://github.com/shivkarankagne/Lisa/releases), or build it
(below). Until the binary is signed, macOS asks the first time:
right-click it in Finder → **Open** → **Open**.

    chmod +x lisa
    ./lisa --version

**2. Get the models** (about 2.5 GB, once). LISA answers with a chat model
and searches with an embedding model:

    mkdir -p ~/lisa-models && cd ~/lisa-models
    curl -L -O https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/bc640142c66e1fdd12af0bd68f40445458f3869b/Qwen3-4B-Q4_K_M.gguf
    curl -L -O https://huggingface.co/ggml-org/e5-small-v2-Q8_0-GGUF/resolve/main/e5-small-v2-q8_0.gguf
    lisa model --set ~/lisa-models/Qwen3-4B-Q4_K_M.gguf
    lisa model --set ~/lisa-models/e5-small-v2-q8_0.gguf
    lisa model            # shows both files and verifies them (SHA-256)

**3. Add documents** (`.txt`, `.md`, `.pdf`, `.docx`; scanned PDFs are
read with text recognition):

    lisa ingest --collection manuals ~/Documents/manuals

**4. Ask:**

    lisa ask --collection manuals "What is the warranty period?"

**Or use the window:**

    lisa gui

The first time, it asks which folders to keep ready — Documents, Desktop
and Downloads are offered, and you can add any other. From then on LISA
keeps them indexed while it is open: new and changed files are picked up
within a minute, so you just ask. Nothing runs when LISA is closed.

---

## What it does

- **Reads** text, Markdown, PDF and Word files, including scanned PDFs
  (text recognition on macOS).
- **Searches** by meaning and by keyword together, so exact terms like
  part numbers work as well as questions in plain language.
- **Answers** with citations: file, page, and the quoted passage. If your
  documents do not contain the answer, it says so instead of guessing.
- **Keeps up to date by itself**: the folders you chose are re-checked
  while LISA is open, and only new or changed files are read. On the
  command line, `lisa ingest` does the same on demand.
- **Works offline**, always. Turn off Wi-Fi and nothing changes.
- **Speaks many languages**, including Hindi and other Indian languages
  in text documents.

Three ways in, all the same engine: the command line, a desktop window,
and a local HTTP API (`docs/http-api.md`) for your own scripts.

## Where your data lives

    ~/Library/Application Support/LISA/
      config.json            which model files to use, and which folders to keep indexed
      collections/<name>/    the index for one set of documents
      lisa.log               what LISA did (never your document text)

Your original files are never modified or copied; LISA stores extracted
text and vectors in the collection. Use `--data <dir>` for a different
location, and delete a collection folder to remove it.

## Build from source

Apple Silicon Mac, CMake 3.20+, Xcode command line tools.

    scripts/fetch_pdfium.sh          # once: prebuilt static PDFium (SHA-256 verified)
    cmake -S . -B build
    cmake --build build -j8
    ctest --test-dir build --output-on-failure

`build/lisa` links only macOS system libraries; everything else
(SQLite, llama.cpp, PDFium, CivetWeb, yyjson, miniz, webview, md4c,
utf8proc) is statically linked. Model tests need the model files; they
report as ignored without them.

## Documentation

| | |
| :--- | :--- |
| Command line | [src/cli/README.md](src/cli/README.md) |
| HTTP API | [docs/http-api.md](docs/http-api.md) |
| Models (and how to verify them) | [docs/models.md](docs/models.md) |
| Collection format | [docs/formats/collection-v2.md](docs/formats/collection-v2.md) |
| Plan and decisions | [LISA_COMPLETION_PLAN.md](LISA_COMPLETION_PLAN.md) |
| Reports for each work package | `LISA_REPORT_AND_UPDATE_*.md` |
| Security | [SECURITY.md](SECURITY.md) |
| Contributing | [CONTRIBUTING.md](CONTRIBUTING.md) |

## Licence

Source-available under the **Business Source License 1.1** (see
[LICENSE](LICENSE)): free for individuals' personal use; organisations
need a commercial licence; each version becomes Apache-2.0 /
GPL-2.0-or-later four years after its release.

> The licence text is a draft and has not yet been reviewed by a lawyer.
> If you plan to rely on it, ask first: open an issue.

Third-party components keep their own licences
([third_party/README.md](third_party/README.md), `NOTICE`).
