#!/usr/bin/env bash
# macOS file names ignore case: a file "VERSION" in an include folder replaced libc++ <version> and broke the Mac build.
# Standard C++ headers have no extension and ours always have one, so any file without an extension in an include
# folder can shadow a standard header. The folders are read from CMakeLists.txt, so a new folder is checked too.
set -u
root="${1:-.}"
dirs=$(grep -oE 'include_directories\([^)]*\)' "$root/CMakeLists.txt" | grep -oE '(src|third_party|tests)[^ )]*' | sort -u)
[ -n "$dirs" ] || { echo "FAIL: no include folders found in CMakeLists.txt"; exit 1; }
bad=0; n=0
for d in $dirs; do
  n=$((n + 1))
  while IFS= read -r f; do
    echo "FAIL: $f has no extension and can shadow a standard header on macOS"; bad=1
  done < <(find "$root/$d" -type f ! -name '*.*')
done
echo "checked $n include folders: $(echo $dirs)"
exit $bad
