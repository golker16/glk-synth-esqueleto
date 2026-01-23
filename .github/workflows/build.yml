name: build-windows

on:
  push:
  pull_request:

jobs:
  build:
    runs-on: windows-latest

    steps:
      - name: Checkout (submodules + LFS)
        uses: actions/checkout@v4
        with:
          fetch-depth: 0
          submodules: recursive
          lfs: true

      - name: Verify assets (font)
        shell: pwsh
        run: |
          Write-Host "Workspace:" $env:GITHUB_WORKSPACE
          Write-Host "Listing assets folder (if exists)..."
          if (!(Test-Path "assets")) {
            throw "Missing folder: assets/ (commit it to the repo)"
          }

          Get-ChildItem -Recurse "assets" | Format-Table FullName, Length

          if (!(Test-Path "assets/mi_fuente.ttf")) {
            throw "Missing file: assets/mi_fuente.ttf (not committed, ignored, or LFS not pulled)"
          }

          $f = Get-Item "assets/mi_fuente.ttf"
          if ($f.Length -lt 1024) {
            Write-Host "Font file is suspiciously small (<1KB). If you use Git LFS, ensure lfs: true is enabled."
          }
          Write-Host "Font OK:" $f.FullName "Size:" $f.Length "bytes"

      - name: Configure (CMake)
        shell: pwsh
        run: |
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

      - name: Build (Release)
        shell: pwsh
        run: |
          cmake --build build --config Release --parallel --verbose

      - name: Upload VST3 artifact
        uses: actions/upload-artifact@v4
        with:
          name: BasicInstrument-Windows-VST3
          if-no-files-found: error
          path: |
            build/**/BasicInstrument*.vst3*
            build/**/Release/**/BasicInstrument*.vst3*
            build/**/VST3/**/BasicInstrument*.vst3*
            build/**/*_artefacts/**/VST3/**/BasicInstrument*.vst3*

