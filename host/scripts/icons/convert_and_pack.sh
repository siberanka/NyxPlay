#!/bin/bash
# NyxPlay Host icons are generated from the artwork in branding/source by the
# repository wide branding generator (requires Python 3, Pillow and numpy).
set -e
cd "$(dirname "$0")/../../.."
exec python3 branding/tools/generate_branding.py "$@"
