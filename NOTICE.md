# Licenses and attribution

Original Windows LHDC code is licensed under the [MIT License](LICENSE).
The following components retain their own licenses:

| Component | License | Source |
| --- | --- | --- |
| `third_party/lhdcv5` | [Apache-2.0](licenses/Apache-2.0.txt) | WillyBilly06 / LHDC-V5-Encoder, revision `3f9d1980fa9c57cdfb78792f9455a7dbac50edda` |
| `third_party/windows-driver-samples` | [MS-PL](licenses/MS-PL.txt) | Copyright (c) Microsoft Corporation, revision `2dc3fd3a0cc84a2933f2194e7ec0871584979071` |
| `drivers/audio/minipairs.h`, `drivers/audio/speakerwavtable.h` | [MS-PL](licenses/MS-PL.txt) | Modified from Microsoft SimpleAudioSample; original copyright notices retained |

`scripts/Build.ps1 -Target Audio` prepares a modified SimpleAudioSample framework
under the ignored `build/audio-reference` directory. That framework retains
MS-PL and the upstream copyright notices. The modifications replace the sample
paths with the LHDC render path, PCM export, format selection and AVRCP volume
support. The upstream submodules themselves are unchanged.

Product names identify tested devices and upstream sources; this project is
not affiliated with Microsoft, OPPO or the LHDC trademark owners.

The HeyMelody wire protocol facts (service UUIDs, frame layout, command IDs and
Hi-Res feature ID `0x18`) were researched using
[OppoPodsWindows](https://github.com/3295074384/OppoPodsWindows) and the
[upstream protocol notes](https://github.com/Leaf-lsgtky/OppoPods/blob/master/docs/HeyMelody_Official_App_Protocol_Findings.md),
then verified against an Enco X4. Those projects retain GPL-3.0; their source
code and device model databases are not included here. `src/control/` is an
independent implementation of the observed wire protocol under this project's MIT license.
