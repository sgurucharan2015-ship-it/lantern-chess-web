# Lantern Chess Studio — automatic CLOUD build + automatic website publishing

**Goal:** Publish the actual Lantern 16.4 C++ engine, 1.99-billion-position `full_best.nnue`, H50, 3 search threads, 512 MB hash, polished two-player / vs-Lantern browser UI. No Emscripten installation on the owner's PC. No visitor uploads, installs, engine backend or API key.

## Status (read this first)

- **This ZIP is a cloud-build-ready repository, not a precompiled deployed website.** The current environment has no Emscripten compiler and cannot verify the actual WebAssembly compilation or live engine play. CI is configured to compile, validate, and deploy when GitHub Actions runs.
- The website runs **the original Lantern C++ move generator, search, and PSN1 NNUE weights**, not a weaker alternative engine. It deliberately does not fall back to JavaScript chess search.
- **3350 Elo (or identical native performance) is NOT guaranteed**. This is a reported/estimated native strength, not a browser measurement. WebAssembly speed differs between browsers/devices, so the same engine can search fewer nodes in the same time. Preserving code/weights/threads/hash does not preserve identical Elo.
- The engine and NNUE files are published publicly. Only deploy the network if you have permission to redistribute it.

## Minimal one-time setup — no installing compilers

1. **Connect GitHub** and create a repository named `lantern-chess-web` with these repository files, including the hidden `.github/workflows/build-and-deploy.yml` file. The repository may be public; make sure you have redistribution rights for the NNUE weights.
2. In that repository open **Settings → Pages** and choose **Source: GitHub Actions**. (One-time GitHub authorization / configuration is unavoidable; ChatGPT cannot silently create a repository or deploy a public website without access to your GitHub account.)
3. On the **Actions** tab, open **Build real Lantern and publish website** and select **Run workflow**, or push any change to `main`.
4. Wait for the successful **build → deploy** workflow. GitHub Pages displays your site's URL (usually `https://GITHUB_USERNAME.github.io/lantern-chess-web/`). This URL is *not assigned until you deploy.*

After this, future pushes to `main` automatically build the engine in a Linux GitHub runner with Emscripten, validate assets and publish the site. **Nothing is installed on your computer.**

### What GitHub does automatically

- Checks out your Lantern C++ source and included NNUE network.
- Installs Emscripten **4.0.7 in a temporary GitHub Actions runner** (not on your PC).
- Builds the actual Lantern search and NNUE evaluator for WebAssembly with three threads and 512 MB hash.
- Verifies NNUE header, web assets, JS syntax and generated WASM/data/worker files.
- Creates a downloadable compiled-site workflow artifact as well as an automatic GitHub Pages deployment.

### 3-thread browser isolation

Multithreaded WebAssembly needs a **cross-origin isolated** page. GitHub Pages cannot set arbitrary HTTP headers, so the kit includes a same-origin service worker shim to attach the necessary COOP/COEP headers on page navigation. On the first visit, it may reload automatically once; subsequent visits should be normal. On hosts that support `_headers` (Cloudflare Pages), use the included `site/_headers` directly, without needing the shim. The shim cannot guarantee every browser supports 3-thread WebAssembly; browsers that lack required capabilities will show a clear error rather than silently reducing Lantern's search strength.

### What visitors see

- Polished sharp-edged chessboard and detailed SVG piece artwork
- Green selected square, legal move dots, capture rings
- Click-to-move and drag-and-drop, animated pieces and highlighted checking king
- Both-sides manual chess **and** Play White vs Lantern **and** Play Black vs Lantern
- FEN loading/copying, move history, promotion chooser, analysis PV and evaluation
- Fully static website: engine calculations happen in each person's browser

### Other hosts

For Cloudflare Pages or another provider that supports COOP+COEP, you can deploy the `site/` folder **after CI builds it**. The `_headers` file is included. Cloudflare credentials/account setup are not built into this GitHub Pages workflow (no secret tokens needed for GitHub Pages).

### Local verification

With Python and Node.js already available:

```bash
python verify_package.py
python tests/check_release.py
```

These check project structure and JavaScript syntax, **not WebAssembly runtime correctness**. The stronger `--require-wasm` flag runs in CI after compilation:

```bash
python verify_package.py --require-wasm
```

### Files

- `.github/workflows/build-and-deploy.yml` — entire hands-off build/deploy process
- `src/lantern_web.cpp` — original engine code plus browser interface
- `src/lantern16_original.cpp` — original Lantern engine preserved for reference
- `site/` — web GUI, textures, NNUE and cross-origin isolation helper
- `build_web.sh` — automated Emscripten build invoked by GitHub Actions
- `tests/check_release.py` — release sanity checks

**Troubleshooting:** If GitHub reports a build failure, check its workflow logs rather than publishing the incomplete source `site/` folder. If a visitor's device lacks cross-origin isolation, browser threads, SIMD, or sufficient RAM, the engine may not work on that device. Guaranteeing a specific published Elo for every visitor is not technically possible under the browser-only and free-hosting constraints.
