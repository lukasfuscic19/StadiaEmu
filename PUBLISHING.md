# Publishing to GitHub

Prepared locally; **push requires your GitHub account** (GitHub CLI `gh` is not installed on this PC).

## 1. Create a new repository

On GitHub: **New repository** → name e.g. `StadiaEmu` → Public → no README (we have one).

## 2. Point git remote to your repo

```powershell
cd D:\Cursor\StadiaEmu

# Replace YOUR_USER with your GitHub username
git remote rename origin upstream
git remote add origin https://github.com/lukasfuscic19/StadiaEmu.git
```

Keep `upstream` if you want to pull from walkco/stadia-vigem later.

## 3. Clean commit (review first)

Do **not** commit secrets or build artifacts:

- `bin/*.exe`, `bin/*.pfx`, `bin/*.cer`, `bin/*.msix`, `bin/*.log`
- `_CL_*` (compiler temp)

Then:

```powershell
git add README.md Install.ps1 PUBLISHING.md .gitignore .github/
git add libstadia/ stadia-vigem/ build_quick.bat Build.ps1 LICENSE
git add ViGEmClient
git status
git commit -m "Stadia ViGEm: installer, README, GitHub release workflow"
git push -u origin master
```

## 4. Create a release

After push, tag a release (GitHub Actions builds the exe):

```powershell
git tag v1.0.0
git push origin v1.0.0
```

Or: GitHub → Releases → Draft new release → tag `v1.0.0` → attach `bin\stadia-vigem-x64.exe` + `Install.ps1` manually for the first release.

## 5. Optional: install GitHub CLI

```powershell
winget install GitHub.cli
gh auth login
gh repo create StadiaEmu --public --source=. --push
```

## Current git state (2026-05-31)

- Remote was: `https://github.com/walkco/stadia-vigem.git` (upstream)
- Local branch `master` ahead with BLE/HidHide/installer changes
- Agent has **no write access** to your GitHub until you authenticate (`gh auth login` or HTTPS + PAT)
