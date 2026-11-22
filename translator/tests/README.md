# Translator tests

`Translator.Tests` covers the decoder, IR/SSA, code generation helpers, parsers with synthetic
inputs, project-manifest loading, DOL-only image construction, build sharding, the runtime native
index, and generic entry-point translation.

The suite needs no game data and no host C++ compiler. Two tests read files from this repository:
`ProductionTranslationPolicyTests` checks `projects/opuc1/recomp.yml`, and
`RuntimeNativeIndexTests.SerializedRepositoryIndexRoundTripsExactly` scans `runtime/src`.

An external generic DOL can be included without adding it to the repository:

```powershell
$env:RECOMP_GENERIC_DOL = 'D:\path\to\main.dol'
dotnet test translator/Translator.sln -c Release
```

The test treats the file as a generic DOL: it reads the DOL entry point, builds a DOL-only image,
and translates that entry point without game-specific addresses or bindings.
