# SVP v1.0 RC2 Release Package

SVP v1.0 RC2 is the active public specification. It adds first-class OCR / visible-text observations and structured color observations as Core sections and defines the authoritative reference-model set.

Start with:

- `spec/SVP_v1_0_RC2.md`
- `spec/companion/SVP_Model_Bundle_v1_RC2.md`
- `spec/registries/reference-model-set.json`
- `spec/review/SVP_v1_0_RC2_Review_Notes.md`
- `docs/SVP_v1_0_RC2_Repo_Update_Handoff.md`

The Markdown, DOCX, and PDF copies of the RC2 specification in this package are generated together from `spec/SVP_v1_0_RC2.md` by `scripts/generate-rc2-release-artifacts.py`.

The deterministic release-artifact environment is Apple Silicon (`Darwin`
`arm64`) with LibreOfficeDev `26.8.0.0.alpha0`, build
`2c87e51eeaa2b413ff4ae097b2705eea1995d8e5`. The recorded generation run used
macOS 27.0 build `26A5378j`. The generator rejects a different operating-system
family, processor architecture, or LibreOffice build instead of silently
producing different release files.
