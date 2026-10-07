# Licenses and attribution

Original Windows LHDC code is licensed under the [MIT License](LICENSE).
The following components retain their own licenses:

| Component | License | Source |
| --- | --- | --- |
| `third_party/lhdcv5` | [Apache-2.0](licenses/Apache-2.0.txt) | WillyBilly06 / LHDC-V5-Encoder, revision `3f9d1980fa9c57cdfb78792f9455a7dbac50edda` |
| `third_party/windows-driver-samples` | [MS-PL](licenses/MS-PL.txt) | Copyright (c) Microsoft Corporation, revision `2dc3fd3a0cc84a2933f2194e7ec0871584979071` |
| `drivers/audio/minipairs.h`, `drivers/audio/speakerwavtable.h` | [MS-PL](licenses/MS-PL.txt) | Modified from Microsoft SimpleAudioSample; original copyright notices retained |

`scripts/Build-AudioDriver.ps1` prepares a modified SimpleAudioSample framework
under the ignored `build/audio-reference` directory. That framework retains
MS-PL and the upstream copyright notices. The modifications replace the sample
paths with the LHDC render path, PCM export, format selection and AVRCP volume
support. The upstream submodules themselves are unchanged.

Product names identify tested devices and upstream sources; this project is
not affiliated with Microsoft, OPPO or the LHDC trademark owners.
