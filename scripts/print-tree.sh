#!/usr/bin/env bash
set -euo pipefail

find . \
  -path "./.git" -prune -o \
  -path "./.tmp" -prune -o \
  -print | sort
