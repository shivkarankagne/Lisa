#!/bin/bash
# SPDX-License-Identifier: BUSL-1.1
# fetch_corpus.sh — download the public half of the W0/W12 corpus.
#
# Public-domain US government publications (IRS, govinfo) and Project
# Gutenberg texts: documents anyone can fetch, so the benchmark can be
# repeated. Each file is recorded with its SHA-256 in corpus.sha256 on
# the first run and verified on later runs.
#
#   benchmark/w0/fetch_corpus.sh [<dir>]     (default: benchmark/w0/corpus)
set -euo pipefail
cd "$(dirname "$0")"
DIR=${1:-corpus}
mkdir -p "$DIR/public"

# Download to <name>.<ext> and keep it only if it really is that format:
# these sites answer a missing document with an HTML page and status 200.
get() {  # <url> <name> <ext> <expected file(1) word>
    local out="$DIR/public/$2.$3"
    [ -f "$out" ] && return 0
    curl -sfL --retry 3 -o "$out.part" "$1" || { echo "    ! $2: download failed"; rm -f "$out.part"; return 1; }
    if ! file -b "$out.part" | grep -qi "$4"; then
        echo "    ! $2: not a $4 (the source moved); skipped"
        rm -f "$out.part"
        return 1
    fi
    mv "$out.part" "$out"
}

irs() {  # <file> <name>
    get "https://www.irs.gov/pub/irs-pdf/$1.pdf" "$2" pdf "PDF"
}
cfr() {  # <title> <vol> <section> <name>
    get "https://www.govinfo.gov/content/pkg/CFR-2023-title$1-vol$2/pdf/CFR-2023-title$1-vol$2-sec$3.pdf" \
        "$4" pdf "PDF"
}
gut() {  # <id> <name>
    get "https://www.gutenberg.org/files/$1/$1-0.txt" "$2" txt "text"
}
echo "==> IRS publications (public domain)"
irs p15    employers-tax-guide
irs p17    your-federal-income-tax
irs p501   dependents-standard-deduction
irs p502   medical-dental-expenses
irs p503   child-dependent-care
irs p505   tax-withholding-estimated-tax
irs p509   tax-calendars
irs p525   taxable-nontaxable-income
irs p527   residential-rental-property
irs p590a  iras-contributions
irs p596   earned-income-credit
irs p946   depreciating-property
irs i1040gi form-1040-instructions
irs p970   education-tax-benefits
irs p554   older-americans-tax-guide

echo "==> Code of Federal Regulations (public domain)"
cfr 29 5  1910-134  osha-respiratory-protection
cfr 29 5  1910-147  osha-lockout-tagout
cfr 21 4  211-22    fda-quality-control-unit
cfr 40 28 261-3     epa-hazardous-waste
cfr 49 5  393-75    dot-tires
cfr 14 2  91-103    faa-preflight-action

echo "==> Project Gutenberg (public domain)"
gut 1342 pride-and-prejudice
gut 11   alices-adventures-in-wonderland
gut 84   frankenstein
gut 1661 adventures-of-sherlock-holmes
gut 2701 moby-dick

n=$(ls "$DIR/public" | wc -l | tr -d ' ')
echo "==> $n public documents in $DIR/public"

if [ -f corpus.sha256 ]; then
    echo "==> Verifying against corpus.sha256"
    ( cd "$DIR" && shasum -a 256 -c ../corpus.sha256 --quiet ) && echo "    all files match"
else
    ( cd "$DIR" && find public -type f | sort | xargs shasum -a 256 ) > corpus.sha256
    echo "==> Wrote corpus.sha256 (commit it so later runs use the same files)"
fi
