Import("env")
import os

# Liest das OTA-Passwort aus ota_pass.txt (gitignored).
# Datei anlegen/aktualisieren wenn OTA-Passwort in der Web-UI geändert wurde.
# Fallback: "admin"

pass_file = os.path.join(env.subst("$PROJECT_DIR"), "ota_pass.txt")
if os.path.isfile(pass_file):
    with open(pass_file) as f:
        ota_pass = f.read().strip()
else:
    ota_pass = "admin"
    with open(pass_file, "w") as f:
        f.write(ota_pass + "\n")
    print(f"[read_ota_pass] ota_pass.txt angelegt mit Standardpasswort 'admin'")

print(f"[read_ota_pass] OTA-Auth: {ota_pass}")
flags = env.get("UPLOAD_FLAGS", [])
flags = [f for f in flags if not str(f).startswith("--auth")]
flags.append(f"--auth={ota_pass}")
env.Replace(UPLOAD_FLAGS=flags)
