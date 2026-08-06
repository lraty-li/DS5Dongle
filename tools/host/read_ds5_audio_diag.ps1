<#
.SYNOPSIS
Read the BL616 DS5 audio diagnostic HID Feature report (0xF6).

.DESCRIPTION
Uses only Windows SetupAPI and hid.dll. It only performs HID GetFeature and
does not modify device state or send output reports.
#>
[CmdletBinding()]
param(
    [switch]$Raw
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not ('Ds5AudioDiagnostics.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace Ds5AudioDiagnostics {
    public static class Native {
        public const uint DIGCF_PRESENT = 0x00000002;
        public const uint DIGCF_DEVICEINTERFACE = 0x00000010;
        public const uint GENERIC_READ = 0x80000000;
        public const uint GENERIC_WRITE = 0x40000000;
        public const uint FILE_SHARE_READ = 0x00000001;
        public const uint FILE_SHARE_WRITE = 0x00000002;
        public const uint OPEN_EXISTING = 3;
        public const uint FILE_ATTRIBUTE_NORMAL = 0x00000080;

        [StructLayout(LayoutKind.Sequential)]
        public struct DeviceInterfaceData {
            public int cbSize;
            public Guid InterfaceClassGuid;
            public int Flags;
            public IntPtr Reserved;
        }

        [DllImport("hid.dll")]
        public static extern void HidD_GetHidGuid(out Guid hidGuid);

        [DllImport("hid.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool HidD_GetFeature(
            IntPtr device, [In, Out] byte[] report, int reportLength);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr SetupDiGetClassDevs(
            ref Guid classGuid, IntPtr enumerator, IntPtr parent, uint flags);

        [DllImport("setupapi.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetupDiEnumDeviceInterfaces(
            IntPtr set, IntPtr deviceInfo, ref Guid classGuid, uint index,
            ref DeviceInterfaceData interfaceData);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetupDiGetDeviceInterfaceDetail(
            IntPtr set, ref DeviceInterfaceData interfaceData, IntPtr detailData,
            uint detailDataSize, out uint requiredSize, IntPtr deviceInfo);

        [DllImport("setupapi.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateFile(
            string name, uint access, uint share, IntPtr security,
            uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool CloseHandle(IntPtr handle);
    }
}
'@
}

function Get-Ds5HidPath {
    $hidGuid = [Guid]::Empty
    [Ds5AudioDiagnostics.Native]::HidD_GetHidGuid([ref]$hidGuid)
    $flags = [Ds5AudioDiagnostics.Native]::DIGCF_PRESENT -bor
        [Ds5AudioDiagnostics.Native]::DIGCF_DEVICEINTERFACE
    $set = [Ds5AudioDiagnostics.Native]::SetupDiGetClassDevs(
        [ref]$hidGuid, [IntPtr]::Zero, [IntPtr]::Zero, $flags)
    if ($set -eq [IntPtr](-1)) {
        throw "SetupDiGetClassDevs failed: Win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }

    try {
        for ([uint32]$index = 0; ; ++$index) {
            $item = New-Object Ds5AudioDiagnostics.Native+DeviceInterfaceData
            $item.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($item)
            if (-not [Ds5AudioDiagnostics.Native]::SetupDiEnumDeviceInterfaces(
                    $set, [IntPtr]::Zero, [ref]$hidGuid, $index, [ref]$item)) {
                $error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                if ($error -eq 259) { break }
                throw "SetupDiEnumDeviceInterfaces failed: Win32=$error"
            }

            [uint32]$required = 0
            $null = [Ds5AudioDiagnostics.Native]::SetupDiGetDeviceInterfaceDetail(
                $set, [ref]$item, [IntPtr]::Zero, 0, [ref]$required, [IntPtr]::Zero)
            if (($required -eq 0) -or ([Runtime.InteropServices.Marshal]::GetLastWin32Error() -ne 122)) {
                throw "SetupDiGetDeviceInterfaceDetail size query failed: Win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
            }

            $detail = [Runtime.InteropServices.Marshal]::AllocHGlobal([int]$required)
            try {
                if ([IntPtr]::Size -eq 8) {
                    [Runtime.InteropServices.Marshal]::WriteInt32($detail, 8)
                }
                else {
                    [Runtime.InteropServices.Marshal]::WriteInt32($detail, 6)
                }
                if (-not [Ds5AudioDiagnostics.Native]::SetupDiGetDeviceInterfaceDetail(
                        $set, [ref]$item, $detail, $required, [ref]$required, [IntPtr]::Zero)) {
                    throw "SetupDiGetDeviceInterfaceDetail failed: Win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
                }
                $path = [Runtime.InteropServices.Marshal]::PtrToStringUni([IntPtr]::Add($detail, 4))
                if ($path -and $path.ToLowerInvariant().Contains('vid_054c&pid_0ce6&mi_03')) {
                    return $path
                }
            }
            finally {
                [Runtime.InteropServices.Marshal]::FreeHGlobal($detail)
            }
        }
    }
    finally {
        $null = [Ds5AudioDiagnostics.Native]::SetupDiDestroyDeviceInfoList($set)
    }

    throw 'DualSense HID interface VID_054C&PID_0CE6&MI_03 was not found.'
}

$path = Get-Ds5HidPath
$access = [Ds5AudioDiagnostics.Native]::GENERIC_READ -bor [Ds5AudioDiagnostics.Native]::GENERIC_WRITE
$share = [Ds5AudioDiagnostics.Native]::FILE_SHARE_READ -bor [Ds5AudioDiagnostics.Native]::FILE_SHARE_WRITE
$handle = [Ds5AudioDiagnostics.Native]::CreateFile(
    $path, $access, $share, [IntPtr]::Zero,
    [Ds5AudioDiagnostics.Native]::OPEN_EXISTING,
    [Ds5AudioDiagnostics.Native]::FILE_ATTRIBUTE_NORMAL, [IntPtr]::Zero)
if ($handle -eq [IntPtr](-1)) {
    throw "CreateFile failed: Win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}

try {
    # HidD_GetFeature supplies the report payload here; the report ID is
    # selected by the HID API call and is not included in this buffer for
    # this CherryUSB device.
    $report = [byte[]]::new(64)
    $report[0] = 0xf6
    if (-not [Ds5AudioDiagnostics.Native]::HidD_GetFeature($handle, $report, $report.Length)) {
        throw "HidD_GetFeature(0xF6) failed: Win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    if (($report[0] -ne 0x44) -or ($report[1] -ne 0x35) -or
        ($report[2] -ne 0x41) -or ($report[3] -ne 0x44)) {
        throw 'Firmware does not expose audio diagnostic Feature 0xF6. Flash the current build first.'
    }

    $flags = $report[5]
    if ($Raw) {
        $rawBytes = @([byte]0xf6) + @($report)
        Write-Output (($rawBytes | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
    }
    [pscustomobject]@{
        HidPath                    = $path
        FirmwareDiagnosticVersion  = $report[4]
        SpeakerStreamOpen          = [bool]($flags -band 0x01)
        MicrophoneStreamOpen       = [bool]($flags -band 0x02)
        SpeakerReadPending         = [bool]($flags -band 0x04)
        MicrophoneWritePending     = [bool]($flags -band 0x08)
        SpeakerMuted               = [bool]($flags -band 0x10)
        MicrophoneMuted            = [bool]($flags -band 0x20)
        ReceivedUsbAudioPackets    = [BitConverter]::ToUInt32($report, 8)
        InvalidUsbAudioPackets     = [BitConverter]::ToUInt32($report, 12)
        DroppedUsbAudioPackets     = [BitConverter]::ToUInt32($report, 16)
        UsbReadArmFailures         = [BitConverter]::ToUInt32($report, 20)
        PublishedHapticsBlocks     = [BitConverter]::ToUInt32($report, 24)
        PublishedSpeakerOpusFrames = [BitConverter]::ToUInt32($report, 28)
        DecodedMicrophoneFrames    = [BitConverter]::ToUInt32($report, 32)
        MicrophoneWriteFailures    = [BitConverter]::ToUInt32($report, 36)
        AudioGeneration            = [BitConverter]::ToUInt32($report, 40)
        ReceivedFeatureSetReports  = [BitConverter]::ToUInt32($report, 44)
        DroppedFeatureSetReports   = [BitConverter]::ToUInt32($report, 48)
        ForwardedFeatureSetReports = [BitConverter]::ToUInt32($report, 52)
        FailedFeatureSetForwards   = [BitConverter]::ToUInt32($report, 56)
        LastFeatureSetReportId     = ('0x{0:X2}' -f $report[60])
        LastFeatureSetPayloadLength = $report[61]
    }
}
finally {
    $null = [Ds5AudioDiagnostics.Native]::CloseHandle($handle)
}
