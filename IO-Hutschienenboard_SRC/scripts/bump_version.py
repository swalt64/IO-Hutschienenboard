Import("env")
import re, os, sys

# Beim Upload direkt beim Laden des Scripts die Version hochzählen,
# damit die Kompilierung die neue Version einbaut.
if "upload" in sys.argv or any("upload" in str(a) for a in sys.argv):
    vh = os.path.join(env.subst("$PROJECT_DIR"), "include", "version.h")
    with open(vh) as f:
        content = f.read()
    m = re.search(r'"(\d+)\.(\d+)\.(\d+)"', content)
    if m:
        major, minor, patch = m.group(1), m.group(2), int(m.group(3))
        new_ver = f"{major}.{minor}.{patch + 1}"
        content = re.sub(r'"\d+\.\d+\.\d+"', f'"{new_ver}"', content, count=1)
        with open(vh, "w") as f:
            f.write(content)
        print(f"\n*** Version -> {new_ver} ***\n")
