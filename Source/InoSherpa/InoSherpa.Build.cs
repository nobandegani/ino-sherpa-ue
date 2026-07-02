// Copyright Inoland. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class InoSherpa : ModuleRules
{
	public InoSherpa(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Smoke tests use file-static console-command registrations in
		// anonymous namespaces; keep TUs isolated.
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
			}
			);

		// sherpa-onnx C API headers, consumed straight from the vendored
		// submodule (the static release archives ship no headers; the setup
		// script enforces submodule tag == SHERPA_VERSION). PRIVATE include
		// path on purpose: sherpa types never leak past this module --
		// consumers only see the Ino* API in Public/.
		string VendorDir = Path.Combine(PluginDirectory, "Vendor", "sherpa-onnx");
		PrivateIncludePaths.Add(VendorDir); // -> #include "sherpa-onnx/c-api/c-api.h"

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Win64: the official win-x64-static-MD-Release-lib package,
			// staged by SherpaOnnx/scripts/setup-sherpa-onnx.ps1. Everything
			// (sherpa + its CPU-only ONNX Runtime + espeak-ng/kaldi/piper
			// deps) links statically INTO this module's DLL -- no runtime
			// binary to stage, delay-load, or rename. See CLAUDE.md
			// "The one core decision: static everywhere".
			//
			// MD static libs match UE's /MD dynamic CRT. Consuming c-api.h
			// with NEITHER SHERPA_ONNX_BUILD_SHARED_LIBS nor
			// SHERPA_ONNX_BUILD_MAIN_LIB defined yields plain (non-dllimport)
			// declarations -- exactly right for static linking, so no
			// PublicDefinitions here.
			string Win64Dir = Path.Combine(PluginDirectory, "Source", "ThirdParty", "Win64");

			string[] StaticLibs = new string[]
			{
				"sherpa-onnx-c-api.lib",
				"sherpa-onnx-cxx-api.lib",
				"sherpa-onnx-core.lib",
				"sherpa-onnx-fst.lib",
				"sherpa-onnx-fstfar.lib",
				"sherpa-onnx-kaldifst-core.lib",
				"kaldi-decoder-core.lib",
				"kaldi-native-fbank-core.lib",
				"ssentencepiece_core.lib",
				"piper_phonemize.lib",
				"espeak-ng.lib",
				"ucd.lib",
				"kissfft-float.lib",
				"onnxruntime.lib",
			};

			foreach (string Lib in StaticLibs)
			{
				string LibPath = Path.Combine(Win64Dir, Lib);
				if (!File.Exists(LibPath))
				{
					throw new BuildException(
						"InoSherpa: missing staged lib '" + Lib + "'. " +
						"Run Plugins/InoSherpa/SherpaOnnx/scripts/setup-sherpa-onnx.ps1 first.");
				}
				PublicAdditionalLibraries.Add(LibPath);
			}

			// The static onnxruntime.lib may reference Win32 system libs at
			// its link boundary. Start without; uncomment on unresolved
			// __imp_* externals.
			// PublicSystemLibraries.AddRange(new string[] { "advapi32.lib", "ws2_32.lib" });
		}

		// Android (phase 2): static libs self-built from Vendor/sherpa-onnx
		// (SHERPA_ONNX_ENABLE_C_API=ON, JNI=OFF, static ORT). See CLAUDE.md.
		// iOS / macOS (phase 3): official static release artifacts.
	}
}
