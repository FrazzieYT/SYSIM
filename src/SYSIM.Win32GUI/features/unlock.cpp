#include "unlock.h"
#include "core/globals.h"
#include "core/app.h"
#include "ui/widgets.h"
#include "utils/unlock/unlock_tools.h"
#include <shellapi.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <vector>
#include <new>
#include <process.h>

using namespace Gdiplus;

bool g_unlockInProgress = false;

// Глобальные состояния
static bool g_immediateUnlock = true;
static bool g_fixBcdSafeBoot = false;
static bool g_fixAcl = false;
static bool g_fixBoot = false;
static bool g_fixIfeo = false;

// Прямоугольники для интерактивных элементов
static RectF g_checkboxRect;        // "Разблок."
static RectF g_bcdCheckboxRect;     // "BCD safeboot"
static RectF g_aclCheckboxRect;     // "ACL"
static RectF g_bootCheckboxRect;    // "Boot"
static RectF g_ifeoCheckboxRect;    // "IFEO"
static RectF g_buttonRect;          // Кнопка "Выполнить"
static RectF g_refreshButtonRect;   // Кнопка "Обновить"
static RectF g_ifeoScanRect;        // Сканирование IFEO
static RectF g_ifeoDeleteRect;      // Удаление IFEO Debugger
std::wstring g_lastReport;   // Текст отчёта / лога

static const wchar_t* RECOVERY_SCRIPT = LR"PS(
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Backup = $RecoveryBackupRoot
$DiagnosticOnly = $SYSIM_DiagnosticOnly
$IsWinRE = $SYSIM_IsRecoveryEnvironment -or $env:SystemDrive -eq 'X:' -or
    $env:SystemRoot -match '(?i)^X:\\Windows$' -or
    (Test-Path -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\MiniNT')
$BackupReady = $true
if (!$IsWinRE) {
    if (!$Backup) {
        $BackupReady = $false
        Write-Output 'BACKUP DIRECTORY FAILED: no backup path was configured.'
    } else {
        try { New-Item -ItemType Directory -Path $Backup -Force -ErrorAction Stop | Out-Null }
        catch { $BackupReady = $false; Write-Output "BACKUP DIRECTORY FAILED: $Backup : $_" }
    }
}

function Section([string]$Name) { Write-Output "`r`n===== $Name =====" }
function Native([string]$Name, [string]$Exe, [string[]]$Arguments) {
    Write-Output "--- $Name ---"
    try {
        $result = & $Exe @Arguments 2>&1
        $code = $LASTEXITCODE
        $result | ForEach-Object { Write-Output $_ }
        Write-Output "ExitCode: $code"
    } catch { Write-Output "ERROR: $_" }
}
function Inspect([string]$Name, [scriptblock]$Action) {
    Write-Output "--- $Name ---"
    try { (& $Action | Format-List * | Out-String) | Write-Output }
    catch { Write-Output "ERROR: $_" }
}
function Exists([string]$Path) {
    Write-Output "$Path : $(Test-Path -LiteralPath $Path)"
}
function UnlockRunRestrictions([string]$Path, [string]$Label) {
    $Changed = $false
    foreach ($Name in @('DisallowRun','RestrictRun')) {
        try {
            $Key = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
            if ($null -ne $Key -and $Key.GetValueNames() -contains $Name) {
                Remove-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction Stop
                $Changed = $true
            }
            $ListPath = Join-Path $Path $Name
            if (Test-Path -LiteralPath $ListPath) {
                Remove-Item -LiteralPath $ListPath -Recurse -Force -ErrorAction Stop
                $Changed = $true
            }
        } catch {
            $script:RepairFailure = $true
            Write-Output "RUN POLICY ERROR ($Label): $_"
        }
    }
    if ($Changed) { Write-Output "RUN RESTRICTIONS CLEARED: $Label" }
    return $Changed
}
function UnlockIfeoDebuggers([string]$Path, [string]$Label) {
    $Changed = $false
    if (!(Test-Path -LiteralPath $Path)) { return $false }
    foreach ($Key in (Get-ChildItem -LiteralPath $Path -ErrorAction SilentlyContinue)) {
        try {
            $Values = Get-ItemProperty -LiteralPath $Key.PSPath -Name Debugger -ErrorAction SilentlyContinue
            if ($null -ne $Values) {
                Remove-ItemProperty -LiteralPath $Key.PSPath -Name Debugger -ErrorAction Stop
                $Changed = $true
            }
        } catch {
            $script:RepairFailure = $true
            Write-Output "IFEO DEBUGGER ERROR ($Label, $($Key.PSChildName)): $_"
        }
    }
    if ($Changed) { Write-Output "IFEO DEBUGGERS CLEARED: $Label" }
    return $Changed
}
function UnlockPolicyLockTrees([string]$SoftwareRoot, [string]$Label) {
    $Changed = $false
    foreach ($RelativePath in @('Policies\Microsoft\Windows\Safer','Policies\Microsoft\Windows\SrpV2')) {
        $PolicyPath = Join-Path $SoftwareRoot $RelativePath
        if (Test-Path -LiteralPath $PolicyPath) {
            try {
                Remove-Item -LiteralPath $PolicyPath -Recurse -Force -ErrorAction Stop
                $Changed = $true
                Write-Output "SRP/APPLOCKER POLICY CLEARED: $Label\$RelativePath"
            } catch {
                $script:RepairFailure = $true
                Write-Output "SRP/APPLOCKER POLICY ERROR ($Label\$RelativePath): $_"
            }
        }
    }
    return $Changed
}

Section 'BACKUP BEFORE CHANGES'
$Hives = @('HKLM\SYSTEM','HKLM\SOFTWARE','HKLM\SAM','HKLM\SECURITY','HKU\.DEFAULT')
if (!$IsWinRE -and $BackupReady) {
    foreach ($Hive in $Hives) {
        $File = Join-Path $Backup (($Hive -replace '[\\.]','_') + '.hiv')
        $Result = & reg.exe save $Hive $File /y 2>&1
        $Code = $LASTEXITCODE
        $Result | ForEach-Object { Write-Output $_ }
        if ($Code -ne 0 -or !(Test-Path -LiteralPath $File)) {
            $BackupReady = $false
            Write-Output "BACKUP FAILED: $Hive"
        } else { Write-Output "BACKUP OK: $Hive -> $File" }
    }
}
$WindowsRoot = if ($IsWinRE) { '' } else { $env:SystemRoot }
$WindowsDrive = if ($IsWinRE) { '' } else { $env:SystemDrive }
$OfflineBackupReady = $false
$WindowsDiskNumber = -1
$WindowsPartitionStyle = 'Unknown'
$BootPartitionNumber = -1
$BootTemporarilyMounted = $false
$BootmgrExisted = $false
$SystemHiveRoot = 'HKLM:\SYSTEM'
$SoftwareHiveRoot = 'HKLM:\SOFTWARE'
$CurrentControlSetName = 'CurrentControlSet'
$LoadedRecoveryHives = @()
$LoadedUserHives = @()
$EspDrive = ''
$EspRoot = ''
$BcdStore = ''
$EspTemporarilyMounted = $false
$BootBackupReady = $false
$BootRepairNeeded = $false
$BcdExisted = $false
$BootDirectoryExisted = $false
if ($IsWinRE) {
    $Candidates = @()
    $CandidateRoots = @()
    try {
        $CandidateRoots = @(Get-Volume | Where-Object DriveLetter | ForEach-Object { "$($_.DriveLetter):" })
    } catch {
        Write-Output "Get-Volume unavailable; using filesystem drives: $_"
        $CandidateRoots = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object { $_.Root.TrimEnd('\') })
    }
    foreach ($Drive in $CandidateRoots) {
        if ($Drive -eq $env:SystemDrive) { continue }
        if ((Test-Path "$Drive\Windows\System32\config\SOFTWARE") -and
            (Test-Path "$Drive\Windows\System32\config\SYSTEM") -and
            (Test-Path "$Drive\Windows\System32\ntoskrnl.exe")) { $Candidates += $Drive }
    }
    $SelectedWindowsDrive = $SelectedWindowsDrive.Trim().TrimEnd('\')
    if ($SelectedWindowsDrive -match '^[A-Za-z]:$' -and
        $SelectedWindowsDrive -ne $env:SystemDrive -and
        (Test-Path "$SelectedWindowsDrive\Windows\System32\config\SOFTWARE") -and
        (Test-Path "$SelectedWindowsDrive\Windows\System32\config\SYSTEM") -and
        (Test-Path "$SelectedWindowsDrive\Windows\System32\ntoskrnl.exe") -and
        $Candidates -notcontains $SelectedWindowsDrive) {
        $Candidates += $SelectedWindowsDrive
    }
    if ($SelectedWindowsDrive -and $Candidates -contains $SelectedWindowsDrive) {
        $WindowsDrive = $SelectedWindowsDrive
    } elseif (!$SelectedWindowsDrive -and $Candidates.Count -eq 1) {
        $WindowsDrive = $Candidates[0]
    }
        if (($Candidates -contains $WindowsDrive) -and $WindowsDrive -ne 'X:') {
        $WindowsRoot = Join-Path $WindowsDrive 'Windows'
    }
    )PS" LR"PS(
        $OfflineBackupReady = $false
        try {
            if (!$Backup) { throw 'No backup directory was configured for the selected Windows volume.' }
            New-Item -ItemType Directory -Path $Backup -Force -ErrorAction Stop | Out-Null
            $OfflineBackupReady = $true
        } catch { Write-Output "OFFLINE BACKUP DIRECTORY FAILED: $Backup : $_" }
        foreach ($Hive in @('SYSTEM','SOFTWARE','SAM','SECURITY','DEFAULT')) {
            if (!$OfflineBackupReady) { break }
            try {
                $Source = Join-Path $WindowsRoot "System32\config\$Hive"
                Copy-Item -LiteralPath $Source -Destination (Join-Path $Backup "Offline_$Hive.hiv") -ErrorAction Stop
                Write-Output "OFFLINE BACKUP OK: $Source"
            } catch {
                $OfflineBackupReady = $false
                Write-Output "OFFLINE BACKUP FAILED: $Hive : $_"
            }
        }
        if ($OfflineBackupReady) {
            foreach ($Hive in @('SYSTEM','SOFTWARE')) {
                $MountName = "SYSIM_RECOVERY_$Hive"
                $Source = Join-Path $WindowsRoot "System32\config\$Hive"
                $LoadOutput = & reg.exe load "HKLM\$MountName" $Source 2>&1
                $LoadCode = $LASTEXITCODE
                $LoadOutput | ForEach-Object { Write-Output $_ }
                if ($LoadCode -eq 0) {
                    $LoadedRecoveryHives += $MountName
                    Write-Output "OFFLINE HIVE LOADED: $Hive"
                } else {
                    $OfflineBackupReady = $false
                    Write-Output "OFFLINE HIVE LOAD FAILED: $Hive"
                    break
                }
            }
            if ($OfflineBackupReady -and
                $LoadedRecoveryHives -contains 'SYSIM_RECOVERY_SYSTEM' -and
                $LoadedRecoveryHives -contains 'SYSIM_RECOVERY_SOFTWARE') {
                $SystemHiveRoot = 'HKLM:\SYSIM_RECOVERY_SYSTEM'
                $SoftwareHiveRoot = 'HKLM:\SYSIM_RECOVERY_SOFTWARE'
                try {
                    $SelectPath = Join-Path $SystemHiveRoot 'Select'
                    $CurrentSet = (Get-ItemProperty -LiteralPath $SelectPath -Name Current -ErrorAction Stop).Current
                    $CurrentControlSetName = 'ControlSet{0:D3}' -f [int]$CurrentSet
                } catch { Write-Output "SYSTEM Select key unavailable: $_" }
            }
            if ($OfflineBackupReady -and $SoftwareHiveRoot -ne 'HKLM:\SOFTWARE') {
                $UsersRoot = Join-Path $WindowsRoot 'Users'
                $ProfileListPath = Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion\ProfileList'
                $UserIndex = 0
                foreach ($ProfileKey in (Get-ChildItem -LiteralPath $ProfileListPath -ErrorAction SilentlyContinue)) {
                    $ProfileImage = (Get-ItemProperty -LiteralPath $ProfileKey.PSPath `
                        -Name ProfileImagePath -ErrorAction SilentlyContinue).ProfileImagePath
                    if (!$ProfileImage -or $ProfileImage -notmatch '(?i)\\Users\\[^\\]+$') { continue }
                    $ProfileName = Split-Path $ProfileImage -Leaf
                    $ProfileRoot = Join-Path $UsersRoot $ProfileName
                    $UserHiveFile = Join-Path $ProfileRoot 'NTUSER.DAT'
                    if (!(Test-Path -LiteralPath $UserHiveFile)) { continue }
                    $UserBackup = Join-Path $Backup ("NTUSER_{0:D3}.hiv" -f $UserIndex)
                    try {
                        Copy-Item -LiteralPath $UserHiveFile -Destination $UserBackup -ErrorAction Stop
                        $UserMount = "SYSIM_USER_{0:D3}" -f $UserIndex
                        $UserLoad = & reg.exe load "HKU\$UserMount" $UserHiveFile 2>&1
                        $UserLoadCode = $LASTEXITCODE
                        $UserLoad | ForEach-Object { Write-Output $_ }
                        if ($UserLoadCode -eq 0) {
                            $LoadedUserHives += [pscustomobject]@{Mount=$UserMount;File=$UserHiveFile;Backup=$UserBackup;Profile=$ProfileRoot}
                            Write-Output "USER HIVE LOADED: $ProfileRoot"
                        } else { Write-Output "USER HIVE LOAD FAILED: $ProfileRoot" }
                    } catch { Write-Output "USER HIVE BACKUP FAILED: $ProfileRoot : $_" }
                    $UserIndex++
                }
            }
        }
)PS" LR"PS(
        if ($OfflineBackupReady) {
            try {
                $WindowsPartition = Get-Partition -DriveLetter ([char]$WindowsDrive[0]) -ErrorAction Stop
                $WindowsDiskNumber = $WindowsPartition.DiskNumber
                $WindowsDisk = Get-Disk -Number $WindowsDiskNumber -ErrorAction Stop
                $WindowsPartitionStyle = [string]$WindowsDisk.PartitionStyle
)PS" LR"PS(
                if ($WindowsPartitionStyle -eq 'GPT') {
                    $EspPartitions = @(Get-Partition -DiskNumber $WindowsDiskNumber |
                        Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' })
                    if ($EspPartitions.Count -eq 1) {
                        $EspPartition = $EspPartitions[0]
                        if ($EspPartition.DriveLetter) {
                            $EspDrive = "$($EspPartition.DriveLetter):"
                        } else {
                            $UsedLetters = @(Get-Volume | Where-Object DriveLetter |
                                ForEach-Object { [string]$_.DriveLetter })
                            $FreeLetter = [char[]]'ZYWVUTSRQPONMLKJIHGFED'
                            $FreeLetter = $FreeLetter | Where-Object { $UsedLetters -notcontains [string]$_ } | Select-Object -First 1
                            if ($FreeLetter) {
                                Set-Partition -DiskNumber $EspPartition.DiskNumber `
                                    -PartitionNumber $EspPartition.PartitionNumber `
                                    -NewDriveLetter $FreeLetter -ErrorAction Stop
                                $EspDrive = "$($FreeLetter):"
                                $EspTemporarilyMounted = $true
                                Write-Output "Temporary ESP drive letter assigned: $EspDrive"
                            }
                        }
                        if ($EspDrive) {
                            $EspRoot = "$EspDrive\"
                            $BcdStore = Join-Path $EspRoot 'EFI\Microsoft\Boot\BCD'
                            $BcdExisted = Test-Path -LiteralPath $BcdStore
                            if (Test-Path -LiteralPath $BcdStore) {
                                Copy-Item -LiteralPath $BcdStore -Destination (Join-Path $Backup 'BCD-ESP') -ErrorAction Stop
                            }
                            $BootDirectory = Join-Path $EspRoot 'EFI\Microsoft\Boot'
                            $BootDirectoryExisted = Test-Path -LiteralPath $BootDirectory
                            if (Test-Path -LiteralPath $BootDirectory) {
                                Copy-Item -LiteralPath $BootDirectory -Destination (Join-Path $Backup 'EFI-Microsoft-Boot') -Recurse -Force -ErrorAction Stop
                            }
                            $BootBackupReady = (!(Test-Path -LiteralPath $BcdStore) -or
                                (Test-Path -LiteralPath (Join-Path $Backup 'BCD-ESP'))) -and
                                (!(Test-Path -LiteralPath $BootDirectory) -or
                                (Test-Path -LiteralPath (Join-Path $Backup 'EFI-Microsoft-Boot')))
                            $BootRepairNeeded = !(Test-Path -LiteralPath $BcdStore) -or
                                !(Test-Path -LiteralPath (Join-Path $BootDirectory 'bootmgfw.efi'))
                            if ((Test-Path -LiteralPath $BcdStore) -and $BootBackupReady) {
                                $BcdCheck = & bcdedit.exe /store $BcdStore /enum all 2>&1
                                $BcdCheckCode = $LASTEXITCODE
                                $BcdCheck | ForEach-Object { Write-Output $_ }
                                if ($BcdCheckCode -ne 0 -or (($BcdCheck -join "`n") -notmatch 'winload\.(efi|exe)')) {
                                    $BootRepairNeeded = $true
                                }
                            }
                        }
                    } elseif ($EspPartitions.Count -gt 1) {
                        Write-Output "Multiple ESP partitions on Windows disk $WindowsDiskNumber; boot repair skipped."
                    }
)PS" LR"PS(
                } elseif ($WindowsPartitionStyle -eq 'MBR') {
                    $ActivePartitions = @(Get-Partition -DiskNumber $WindowsDiskNumber |
                        Where-Object { $_.IsActive })
                    if ($ActivePartitions.Count -eq 1) {
                        $BootPartition = $ActivePartitions[0]
                        $BootPartitionNumber = $BootPartition.PartitionNumber
                        if ($BootPartition.DriveLetter) {
                            $BootDrive = "$($BootPartition.DriveLetter):"
                        } else {
                            $UsedLetters = @(Get-Volume | Where-Object DriveLetter |
                                ForEach-Object { [string]$_.DriveLetter })
                            $FreeLetter = [char[]]'ZYWVUTSRQPONMLKJIHGFED'
                            $FreeLetter = $FreeLetter | Where-Object { $UsedLetters -notcontains [string]$_ } | Select-Object -First 1
                            if ($FreeLetter) {
                                Set-Partition -DiskNumber $WindowsDiskNumber `
                                    -PartitionNumber $BootPartitionNumber `
                                    -NewDriveLetter $FreeLetter -ErrorAction Stop
                                $BootDrive = "$($FreeLetter):"
                                $BootTemporarilyMounted = $true
                            }
                        }
                        if ($BootDrive) {
                            $BootRoot = "$BootDrive\"
                            $BootDirectory = Join-Path $BootRoot 'Boot'
                            $BcdStore = Join-Path $BootDirectory 'BCD'
                            $BcdExisted = Test-Path -LiteralPath $BcdStore
                            $BootmgrExisted = Test-Path -LiteralPath (Join-Path $BootRoot 'bootmgr')
                            if ($BcdExisted) {
                                Copy-Item -LiteralPath $BcdStore -Destination (Join-Path $Backup 'BCD-BIOS') -ErrorAction Stop
                            }
                            if (Test-Path -LiteralPath $BootDirectory) {
                                Copy-Item -LiteralPath $BootDirectory -Destination (Join-Path $Backup 'BIOS-Boot') -Recurse -Force -ErrorAction Stop
                            }
                            if ($BootmgrExisted) {
                                Copy-Item -LiteralPath (Join-Path $BootRoot 'bootmgr') -Destination (Join-Path $Backup 'bootmgr') -Force -ErrorAction Stop
                            }
                            $BootBackupReady = (!$BcdExisted -or (Test-Path (Join-Path $Backup 'BCD-BIOS'))) -and
                                (!(Test-Path -LiteralPath $BootDirectory) -or (Test-Path (Join-Path $Backup 'BIOS-Boot'))) -and
                                (!$BootmgrExisted -or (Test-Path (Join-Path $Backup 'bootmgr')))
                            $BootRepairNeeded = !$BcdExisted -or !$BootmgrExisted
                            if ($BcdExisted -and $BootBackupReady) {
                                $BcdCheck = & bcdedit.exe /store $BcdStore /enum all 2>&1
                                $BcdCheckCode = $LASTEXITCODE
                                $BcdCheck | ForEach-Object { Write-Output $_ }
                                if ($BcdCheckCode -ne 0 -or (($BcdCheck -join "`n") -notmatch 'winload\.(efi|exe)')) {
                                    $BootRepairNeeded = $true
                                }
                            }
                        }
                    } else {
                        Write-Output "Active MBR system partitions found: $($ActivePartitions.Count); BIOS boot repair skipped."
                    }
                }
            } catch { Write-Output "Windows disk / EFI discovery failed: $_" }
        }
    } else {
        $OfflineBackupReady = $false
        Write-Output "Offline Windows candidates: $($Candidates.Count); selected target '$SelectedWindowsDrive' is missing or ambiguous."
    }
}
if ($IsWinRE -and !$WindowsDrive) {
    Section 'WINPE TARGET SELECTION'
    Native 'BitLocker state (no unlock attempted)' 'manage-bde.exe' @('-status')
    Native 'Available volume mount points' 'mountvol.exe' @()
    Inspect 'Accessible volumes' { Get-Volume | Select DriveLetter,FileSystem,FileSystemLabel,HealthStatus,OperationalStatus,Size,SizeRemaining }
    Write-Output "Windows candidates found: $($Candidates.Count). No target selected; all repairs were skipped."
    return
}

if (!$IsWinRE) {
    Native 'BCD export' 'bcdedit.exe' @('/export', (Join-Path $Backup 'BCD'))
} elseif ($BcdStore -and (Test-Path -LiteralPath $BcdStore)) {
    Native 'Selected Windows BCD objects' 'bcdedit.exe' @('/store',$BcdStore,'/enum','all')
}
Write-Output "RegistryBackupReady: $BackupReady"
Write-Output "OfflineRegistryBackupReady: $OfflineBackupReady"
Write-Output "WindowsDrive: $WindowsDrive; Disk: $WindowsDiskNumber; PartitionStyle: $WindowsPartitionStyle"
Write-Output "EspDrive: $EspDrive; BootDrive: $BootDrive; BootBackupReady: $BootBackupReady; BootRepairNeeded: $BootRepairNeeded"

)PS" LR"PS(
Section '01 STORAGE / DISK'
Inspect 'Physical disks' { Get-Disk }
Inspect 'Partitions and GPT/MBR' { Get-Partition | Select DiskNumber,PartitionNumber,DriveLetter,Type,GptType,Size }
Inspect 'Volumes, filesystem and free space' { Get-Volume | Select DriveLetter,FileSystem,FileSystemLabel,HealthStatus,OperationalStatus,Size,SizeRemaining }
Inspect 'Physical media health' { Get-PhysicalDisk | Select FriendlyName,MediaType,HealthStatus,OperationalStatus,Size }
Native 'Dirty bit' 'fsutil.exe' @('dirty','query',$WindowsDrive)
Native 'Online filesystem scan' 'chkdsk.exe' @($WindowsDrive,'/scan')

Section '02 BOOT'
if ($IsWinRE -and $BcdStore -and (Test-Path -LiteralPath $BcdStore)) {
    Native 'Selected Windows Boot Manager' 'bcdedit.exe' @('/store',$BcdStore,'/enum','{bootmgr}')
    Native 'Selected Windows BCD objects and loaders' 'bcdedit.exe' @('/store',$BcdStore,'/enum','all')
} else {
    Native 'Boot manager and current loader' 'bcdedit.exe' @('/enum','{bootmgr}')
    Native 'All BCD objects and store' 'bcdedit.exe' @('/enum','all')
}
Exists (Join-Path $WindowsRoot 'bootmgr')
Exists (Join-Path $WindowsRoot 'bootstat.dat')
Inspect 'Firmware boot entries' { Get-CimInstance Win32_BootConfiguration }

Section '03 BITLOCKER'
Native 'BitLocker volume state (no unlock attempted)' 'manage-bde.exe' @('-status')
if ($WindowsDrive -and $WindowsDrive -ne 'X:') {
    Native 'Selected Windows volume protection state' 'manage-bde.exe' @('-status',$WindowsDrive)
}

Section '04 EFI'
Inspect 'EFI system partitions on selected Windows disk' { Get-Partition -DiskNumber $WindowsDiskNumber -ErrorAction Stop | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } }
if ($EspRoot) {
    Exists (Join-Path $EspRoot 'EFI\Microsoft\Boot\bootmgfw.efi')
    Exists (Join-Path $EspRoot 'EFI\Microsoft\Boot\BCD')
} else {
    Write-Output 'A unique accessible ESP on the selected Windows disk was not found.'
}
Native 'Firmware boot mapping' 'bcdedit.exe' @('/enum','firmware')

Section '04 WINRE'
if ($IsWinRE) {
    Native 'Selected Windows Recovery Environment status' 'reagentc.exe' @('/info','/target',$WindowsRoot)
} else { Native 'Windows Recovery Environment status' 'reagentc.exe' @('/info') }
Inspect 'Recovery partitions on selected Windows disk' { Get-Partition -DiskNumber $WindowsDiskNumber -ErrorAction Stop | Where-Object Type -Match 'Recovery' }
Inspect 'WinRE image files' { Get-ChildItem "$WindowsRoot\System32\Recovery" -Filter 'winre.wim' -ErrorAction SilentlyContinue }

Section '05 SYSTEM FILES'
if ($IsWinRE -and $OfflineBackupReady) {
    Native 'Offline protected file verification' 'sfc.exe' @('/verifyonly',"/offbootdir=$WindowsDrive\","/offwindir=$WindowsRoot")
} elseif (!$IsWinRE) {
    Native 'Protected file verification' 'sfc.exe' @('/verifyonly')
} else { Write-Output 'Offline SFC skipped: the selected installation is not backed up and mounted.' }
Exists (Join-Path $WindowsRoot 'System32\kernel32.dll')
Exists (Join-Path $WindowsRoot 'System32\ntoskrnl.exe')
Exists (Join-Path $WindowsRoot 'System32\drivers\ntfs.sys')
Exists (Join-Path $WindowsRoot 'WinSxS\Manifests')

Section '06 COMPONENT STORE'
if ($IsWinRE -and $OfflineBackupReady) {
    Native 'Offline DISM CheckHealth' 'dism.exe' @("/Image:$WindowsDrive\",'/Cleanup-Image','/CheckHealth')
    Native 'Offline DISM ScanHealth' 'dism.exe' @("/Image:$WindowsDrive\",'/Cleanup-Image','/ScanHealth')
    Native 'Offline SFC verification' 'sfc.exe' @('/verifyonly',"/offbootdir=$WindowsDrive\","/offwindir=$WindowsRoot")
} else {
    Native 'DISM CheckHealth' 'dism.exe' @('/Online','/Cleanup-Image','/CheckHealth')
    Native 'DISM ScanHealth' 'dism.exe' @('/Online','/Cleanup-Image','/ScanHealth')
    Native 'SFC verification' 'sfc.exe' @('/verifyonly')
}
Exists (Join-Path $WindowsRoot 'Logs\CBS\CBS.log')

Section '07 WINDOWS SERVICING'
Exists (Join-Path $WindowsRoot 'WinSxS\pending.xml')
Inspect 'Servicing pending keys' { Get-ChildItem (Join-Path $SoftwareHiveRoot 'Microsoft\Windows\CurrentVersion\Component Based Servicing') -ErrorAction SilentlyContinue | Where-Object PSChildName -Match 'Pending|Reboot' }
Inspect 'Session Manager pending operations' { Get-ItemProperty (Join-Path $SystemHiveRoot "$CurrentControlSetName\Control\Session Manager") -Name PendingFileRenameOperations -ErrorAction SilentlyContinue }

Section '08 WINDOWS UPDATE'
foreach ($Service in @('wuauserv','UsoSvc','WaaSMedicSvc','BITS','CryptSvc')) { Native "Service $Service" 'sc.exe' @('query',$Service) }
Exists (Join-Path $WindowsRoot 'SoftwareDistribution')
Exists (Join-Path $WindowsRoot 'System32\catroot2')
Inspect 'Recent update events' { Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Microsoft-Windows-WindowsUpdateClient';StartTime=(Get-Date).AddDays(-14)} -MaxEvents 20 -ErrorAction Stop | Select TimeCreated,Id,LevelDisplayName,Message }

Section '09 DRIVER RECOVERY'
Native 'Driver packages' 'pnputil.exe' @('/enum-drivers')
Native 'Driver services' 'driverquery.exe' @('/v','/fo','table')
Exists (Join-Path $WindowsRoot 'System32\DriverStore\FileRepository')
Exists (Join-Path $WindowsRoot 'System32\drivers')
Inspect 'Code Integrity failures' { Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-CodeIntegrity/Operational';StartTime=(Get-Date).AddDays(-30)} -MaxEvents 30 -ErrorAction Stop | Select TimeCreated,Id,LevelDisplayName,Message }

Section '10 SERVICES'
if ($IsWinRE -and $OfflineBackupReady) {
    $ServicesPath = Join-Path $SystemHiveRoot "$CurrentControlSetName\Services"
    foreach ($Service in @('RpcSs','DcomLaunch','PlugPlay','EventLog','Schedule','TrustedInstaller','WinDefend','BITS')) {
        Inspect "Offline service $Service" { Get-ItemProperty (Join-Path $ServicesPath $Service) -ErrorAction SilentlyContinue | Select Start,Type,ErrorControl,ImagePath,Group,DependOnService,DependOnGroup }
        Inspect "Offline service DLL $Service" { Get-ItemProperty (Join-Path (Join-Path $ServicesPath $Service) 'Parameters') -Name ServiceDll -ErrorAction SilentlyContinue }
    }
    Inspect 'Offline boot-start and system-start drivers' {
        Get-ChildItem $ServicesPath -ErrorAction SilentlyContinue | ForEach-Object {
            $ServiceConfig = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            if ($ServiceConfig.Start -in 0,1) {
                [pscustomobject]@{Name=$_.PSChildName;Start=$ServiceConfig.Start;Type=$ServiceConfig.Type;ErrorControl=$ServiceConfig.ErrorControl;ImagePath=$ServiceConfig.ImagePath;Group=$ServiceConfig.Group;Dependencies=$ServiceConfig.DependOnService}
            }
        }
    }
} else {
    foreach ($Service in @('RpcSs','DcomLaunch','PlugPlay','EventLog','Schedule','TrustedInstaller','WinDefend','BITS')) { Native "Configuration $Service" 'sc.exe' @('qc',$Service); Native "State $Service" 'sc.exe' @('query',$Service) }
}

)PS" LR"PS(
Section '11 REGISTRY'
foreach ($Hive in @('SYSTEM','SOFTWARE','SAM','SECURITY','DEFAULT')) { Exists (Join-Path $WindowsRoot "System32\config\$Hive") }
Inspect 'Registry transaction logs' { Get-ChildItem (Join-Path $WindowsRoot 'System32\config') -Filter '*.LOG*' -ErrorAction SilentlyContinue | Select Name,Length,LastWriteTime }

Section '12 REGISTRY BACKUPS'
Inspect 'RegBack files' { Get-ChildItem (Join-Path $WindowsRoot 'System32\config\RegBack') -ErrorAction SilentlyContinue | Select Name,Length,LastWriteTime }
Write-Output "Current hive backups: $Backup"

Section '13 LOGON'
Inspect 'Selected Windows Winlogon configuration' { Get-ItemProperty (Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion\Winlogon') -ErrorAction SilentlyContinue | Select Shell,Userinit,AutoRestartShell }
Inspect 'Selected Windows user profiles' { Get-ChildItem (Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion\ProfileList') -ErrorAction SilentlyContinue | ForEach-Object { Get-ItemProperty $_.PSPath | Select PSChildName,ProfileImagePath,State } }

Section '14 EXPLORER / SHELL'
Inspect 'Selected Windows shell and Explorer values' { Get-ItemProperty (Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion\Winlogon') -ErrorAction SilentlyContinue | Select Shell,Userinit }
if (!$IsWinRE) { Inspect 'Shell process' { Get-Process explorer -ErrorAction SilentlyContinue | Select Id,StartTime,Path } }

Section '15 AUTOSTART'
foreach ($Path in @('Microsoft\Windows\CurrentVersion\Run','Microsoft\Windows\CurrentVersion\RunOnce','Wow6432Node\Microsoft\Windows\CurrentVersion\Run','Wow6432Node\Microsoft\Windows\CurrentVersion\RunOnce')) { Inspect "Selected Windows SOFTWARE $Path" { Get-ItemProperty (Join-Path $SoftwareHiveRoot $Path) -ErrorAction SilentlyContinue } }
Inspect 'Selected Windows Winlogon autostart' { Get-ItemProperty (Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion\Winlogon') -ErrorAction SilentlyContinue | Select Shell,Userinit,Taskman }
Inspect 'Selected Windows BootExecute' { Get-ItemProperty (Join-Path $SystemHiveRoot "$CurrentControlSetName\Control\Session Manager") -Name BootExecute -ErrorAction SilentlyContinue }
Inspect 'Selected Windows IFEO entries' { Get-ChildItem (Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion\Image File Execution Options') -ErrorAction SilentlyContinue | Select -First 200 PSChildName }
if (!$IsWinRE) { Native 'Scheduled task summary' 'schtasks.exe' @('/query','/fo','table') }
Inspect 'Offline scheduled task files' { Get-ChildItem (Join-Path $WindowsRoot 'System32\Tasks') -File -Recurse -ErrorAction SilentlyContinue | Select -First 200 FullName,LastWriteTime }

Section '16 TASK SCHEDULER'
Native 'Task inventory' 'schtasks.exe' @('/query','/fo','list','/v')
Inspect 'Selected Windows TaskCache' { Get-ChildItem (Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree') -ErrorAction SilentlyContinue | Select PSChildName }

Section '17 SAFE MODE'
Inspect 'Selected Windows SafeBoot configuration' { Get-ChildItem (Join-Path $SystemHiveRoot "$CurrentControlSetName\Control\SafeBoot") -ErrorAction SilentlyContinue | Select PSChildName }
if ($IsWinRE -and $BcdStore) { Native 'Selected Windows BCD safe boot flags' 'bcdedit.exe' @('/store',$BcdStore,'/enum','all') }
elseif (!$IsWinRE) { Native 'BCD safe boot flags' 'bcdedit.exe' @('/enum','{current}') }

Section '18 SECURITY / CODE INTEGRITY'
Inspect 'Security policy services' { Get-Service AppIDSvc,WinDefend,WdFilter -ErrorAction SilentlyContinue | Select Name,Status,StartType }
Inspect 'Code Integrity recent events' { Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-CodeIntegrity/Operational';StartTime=(Get-Date).AddDays(-7)} -MaxEvents 50 -ErrorAction Stop | Select TimeCreated,Id,Message }

Section '19 UAC / SECURITY POLICY'
Inspect 'Selected Windows UAC policy' { Get-ItemProperty (Join-Path $SoftwareHiveRoot 'Microsoft\Windows\CurrentVersion\Policies\System') -ErrorAction SilentlyContinue | Select EnableLUA,ConsentPromptBehaviorAdmin,PromptOnSecureDesktop,FilterAdministratorToken }
Native 'Local security policy export' 'secedit.exe' @('/export','/cfg',(Join-Path $Root 'security-policy.inf'))

Section '20 ACL / PERMISSIONS'
Inspect 'Windows directory ACL' { Get-Acl $WindowsRoot }
Inspect 'System32 directory ACL' { Get-Acl (Join-Path $WindowsRoot 'System32') }
Inspect 'Selected Windows service registry ACL' { Get-Acl (Join-Path $SystemHiveRoot "$CurrentControlSetName\Services") }

)PS" LR"PS(
Section '21 EVENT LOG ANALYSIS'
if ($IsWinRE) {
    $OfflineLogDirectory = Join-Path $WindowsRoot 'System32\winevt\Logs'
    $OfflineLogs = @('System.evtx','Application.evtx','Setup.evtx',
        'Microsoft-Windows-CodeIntegrity%4Operational.evtx',
        'Microsoft-Windows-WindowsUpdateClient%4Operational.evtx',
        'Microsoft-Windows-Kernel-Boot%4Operational.evtx',
        'Microsoft-Windows-Kernel-Power%4Operational.evtx',
        'Microsoft-Windows-User Profile Service%4Operational.evtx')
    foreach ($LogFile in $OfflineLogs) {
        $LogPath = Join-Path $OfflineLogDirectory $LogFile
        if (Test-Path -LiteralPath $LogPath) {
            Inspect "Offline events $LogFile" { Get-WinEvent -Path $LogPath -MaxEvents 40 -ErrorAction Stop | Select TimeCreated,ProviderName,Id,LevelDisplayName,Message }
        }
    }
} else {
    foreach ($LogName in @('System','Application','Setup','Security','Microsoft-Windows-CodeIntegrity/Operational','Microsoft-Windows-Kernel-Boot/Operational')) {
        Inspect "Recent events $LogName" { Get-WinEvent -FilterHashtable @{LogName=$LogName;StartTime=(Get-Date).AddDays(-7)} -MaxEvents 40 -ErrorAction Stop | Select TimeCreated,ProviderName,Id,LevelDisplayName,Message }
    }
}

Section '22 CRASH ANALYSIS'
foreach ($Path in @((Join-Path $WindowsRoot 'Minidump'),(Join-Path $WindowsRoot 'MEMORY.DMP'))) { Inspect "Crash artifacts $Path" { Get-ChildItem $Path -ErrorAction SilentlyContinue | Sort LastWriteTime -Descending | Select -First 20 Name,Length,LastWriteTime } }
Inspect 'Selected Windows BugCheck configuration' { Get-ItemProperty (Join-Path $SystemHiveRoot "$CurrentControlSetName\Control\CrashControl") -ErrorAction SilentlyContinue }

Section '23 SYSTEM RESTORE'
Native 'Restore point inventory' 'vssadmin.exe' @('list','shadows')
Inspect 'Restore points' { Get-ComputerRestorePoint -ErrorAction Stop | Select SequenceNumber,CreationTime,Description }

Section '24 VOLUME SHADOW COPY'
Native 'Shadow storage' 'vssadmin.exe' @('list','shadowstorage')
Native 'Shadow writers' 'vssadmin.exe' @('list','writers')

Section '25 NETWORK'
Native 'Network configuration' 'ipconfig.exe' @('/all')
Native 'Winsock catalog' 'netsh.exe' @('winsock','show','catalog')
Inspect 'Network services' { Get-Service Dhcp,Dnscache,NlaSvc,netprofm -ErrorAction SilentlyContinue | Select Name,Status,StartType }

Section '26 WMI'
if ($IsWinRE) {
    Exists (Join-Path $WindowsRoot 'System32\wbem\Repository\OBJECTS.DATA')
    Inspect 'Selected Windows WMI service configuration' { Get-ItemProperty (Join-Path $SystemHiveRoot "$CurrentControlSetName\Services\Winmgmt") -ErrorAction SilentlyContinue | Select Start,Type,ImagePath }
} else {
    Native 'WMI repository verification' 'winmgmt.exe' @('/verifyrepository')
    Inspect 'WMI service' { Get-Service Winmgmt -ErrorAction SilentlyContinue | Select Name,Status,StartType }
}

)PS" LR"PS(
Section '27 APPX / SYSTEM APPS'
Inspect 'AppX package state' { Get-AppxPackage -AllUsers -ErrorAction Stop | Select Name,PackageFullName,Status }
Inspect 'Deployment services' { Get-Service AppXSvc,ClipSVC,StateRepository -ErrorAction SilentlyContinue | Select Name,Status,StartType }

Section '28 USER ENVIRONMENT'
Inspect 'User environment' { Get-ChildItem Env: | Sort Name }
Inspect 'ProfileList' { Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\*' -ErrorAction SilentlyContinue | Select PSChildName,ProfileImagePath,Flags,State }
Exists (Join-Path $env:USERPROFILE 'NTUSER.DAT')

Section '29 TEMPORARY DAMAGE'
Exists (Join-Path $WindowsRoot 'WinSxS\pending.xml')
Inspect 'Pending operations and servicing logs' { Get-ChildItem "$WindowsRoot\Logs\CBS" -ErrorAction SilentlyContinue | Sort LastWriteTime -Descending | Select -First 20 Name,Length,LastWriteTime }

Section '30 LAST KNOWN STATE'
Inspect 'Recent system events' { Get-WinEvent -FilterHashtable @{LogName='System';StartTime=(Get-Date).AddDays(-3)} -MaxEvents 100 -ErrorAction Stop | Select TimeCreated,ProviderName,Id,LevelDisplayName,Message }
Inspect 'Recent setup and servicing logs' { Get-ChildItem "$WindowsRoot\Logs\DISM" -ErrorAction SilentlyContinue | Sort LastWriteTime -Descending | Select -First 10 Name,Length,LastWriteTime }

Section '33 BOOT FILE INTEGRITY'
foreach ($BootFile in @(
    (Join-Path $WindowsRoot 'System32\winload.exe'),
    (Join-Path $WindowsRoot 'System32\winload.efi'),
    (Join-Path $WindowsRoot 'System32\bootres.dll'),
    (Join-Path $WindowsRoot 'System32\hal.dll'),
    (Join-Path $WindowsRoot 'System32\boot\winload.exe'),
    (Join-Path $WindowsRoot 'System32\boot\winload.efi')) {
    Inspect "Boot file $BootFile" { Get-Item $BootFile -ErrorAction SilentlyContinue | Select FullName,Length,LastWriteTime,VersionInfo }
}
if ($BcdStore -and (Test-Path -LiteralPath $BcdStore)) {
    Native 'BCD store integrity scan' 'bcdedit.exe' @('/store',$BcdStore,'/enum','all','/v')
}

Section '34 BOOT LOGS'
foreach ($BootLog in @('ntbtlog.txt','System32\LogFiles\Srt\SrtTrail.txt','System32\LogFiles\Srt\SrtTrail.txt','Logs\CBS\CBS.log')) {
    $BootLogPath = Join-Path $WindowsRoot $BootLog
    Inspect "Boot log $BootLog" { Get-Item $BootLogPath -ErrorAction SilentlyContinue | Select FullName,Length,LastWriteTime }
}
Inspect 'Recent boot diagnostic events' { Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-Kernel-Boot/Operational';StartTime=(Get-Date).AddDays(-30)} -MaxEvents 100 -ErrorAction Stop | Select TimeCreated,Id,LevelDisplayName,Message }
Inspect 'Recent boot configuration events' { Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-Diagnostics-Performance/Operational';StartTime=(Get-Date).AddDays(-30)} -MaxEvents 50 -ErrorAction Stop | Select TimeCreated,Id,LevelDisplayName,Message }

Section '35 STORAGE HEALTH'
Inspect 'Disk SMART and health data' { Get-PhysicalDisk -ErrorAction Stop | Get-StorageReliabilityCounter -ErrorAction SilentlyContinue | Select DeviceId,Temperature,TemperatureMax,ReadErrorsTotal,WriteErrorsTotal,ReadLatencyMax,WriteLatencyMax,PowerOnHours }
Inspect 'Storage spaces status' { Get-StoragePool -ErrorAction SilentlyContinue | Select FriendlyName,HealthStatus,OperationalStatus,Size,AllocatedSize }
Inspect 'Partition access paths' { Get-Partition -ErrorAction SilentlyContinue | Get-AccessPath -ErrorAction SilentlyContinue | Select DiskNumber,PartitionNumber,AccessPath }
Native 'Volume filesystem details' 'fsutil.exe' @('fsinfo','volumeinfo',$WindowsDrive)
Native 'NTFS statistics' 'fsutil.exe' @('fsinfo','ntfsinfo',$WindowsDrive)

Section '36 FILE SYSTEM METADATA'
Inspect 'Windows directory attributes' { Get-Item $WindowsRoot -Force | Select FullName,Attributes,CreationTime,LastWriteTime }
Inspect 'System32 directory metadata' { Get-Item (Join-Path $WindowsRoot 'System32') -Force | Select FullName,Attributes,CreationTime,LastWriteTime }
Inspect 'System volume information' { Get-ChildItem (Join-Path $WindowsDrive '$Volume') -Force -ErrorAction SilentlyContinue }
Inspect 'Mount points in Windows tree' { Get-ChildItem $WindowsRoot -Directory -Force -ErrorAction SilentlyContinue | Get-Item -Stream * -ErrorAction SilentlyContinue }

Section '37 DRIVER STORE'
Native 'Installed driver packages verbose' 'pnputil.exe' @('/enum-drivers','/class','System')
Native 'Installed boot-critical drivers' 'pnputil.exe' @('/enum-drivers','/class','SCSIAdapter')
Inspect 'Driver store timestamps' { Get-ChildItem (Join-Path $WindowsRoot 'System32\DriverStore\FileRepository') -Directory -Force -ErrorAction SilentlyContinue | Sort LastWriteTime -Descending | Select -First 100 Name,LastWriteTime }
Inspect 'Critical driver files' { Get-ChildItem (Join-Path $WindowsRoot 'System32\drivers') -File -Force -ErrorAction SilentlyContinue | Where-Object Name -Match '^(disk|partmgr|volmgr|volsnap|ntfs|stornvme|storahci|classpnp|mountmgr)\.sys$' | Select Name,Length,LastWriteTime }

Section '38 DEVICE ENUMERATION'
Native 'Present devices' 'pnputil.exe' @('/enum-devices','/connected')
Native 'Problem devices' 'pnputil.exe' @('/enum-devices','/problem')
Inspect 'Offline device setup logs' { Get-ChildItem (Join-Path $WindowsRoot 'INF') -Filter 'setupapi*.log' -ErrorAction SilentlyContinue | Sort LastWriteTime -Descending | Select -First 20 Name,Length,LastWriteTime }

Section '39 SECURITY DATABASE'
Inspect 'Security database files' { Get-ChildItem (Join-Path $WindowsRoot 'Security\Database') -Force -ErrorAction SilentlyContinue | Select Name,Length,LastWriteTime }
Inspect 'Security policy registry areas' { Get-ChildItem (Join-Path $SoftwareHiveRoot 'Microsoft\Windows\CurrentVersion\Policies') -ErrorAction SilentlyContinue | Select PSChildName }
Inspect 'Windows Defender offline configuration' { Get-ItemProperty (Join-Path $SoftwareHiveRoot 'Microsoft\Windows Defender') -ErrorAction SilentlyContinue | Select DisableAntiSpyware,DisableRealtimeMonitoring,ServiceKeepAlive }
Inspect 'LSA configuration' { Get-ItemProperty (Join-Path $SystemHiveRoot "$CurrentControlSetName\Control\Lsa") -ErrorAction SilentlyContinue | Select LsaCfgFlags,RunAsPPL,RunAsPPLBoot,DisableRestrictedAdmin }

)PS" LR"PS(
Section '40 LOGON AND PROFILES'
Inspect 'Profile directories' { Get-ChildItem (Join-Path $WindowsRoot 'Users') -Directory -Force -ErrorAction SilentlyContinue | Select Name,FullName,CreationTime,LastWriteTime }
Inspect 'Profile registry consistency' {
    Get-ChildItem (Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion\ProfileList') -ErrorAction SilentlyContinue |
        ForEach-Object {
            $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            [pscustomobject]@{Sid=$_.PSChildName;ProfileImagePath=$p.ProfileImagePath;
                Exists=(Test-Path $p.ProfileImagePath);State=$p.State}
        }
}
Inspect 'Default profile contents' { Get-ChildItem (Join-Path $WindowsRoot 'Users\Default') -Force -ErrorAction SilentlyContinue | Select Name,Mode,Length,LastWriteTime }
Inspect 'Logon UI policy' { Get-ItemProperty (Join-Path $SoftwareHiveRoot 'Microsoft\Windows\CurrentVersion\Policies\System') -ErrorAction SilentlyContinue | Select DontDisplayLastUserName,DisableLockWorkstation,DisableChangePassword,ShutdownWithoutLogon }

Section '41 NETWORK STACK'
Native 'TCP/IP configuration' 'netsh.exe' @('interface','ipv4','show','config')
Native 'TCP/IP interfaces' 'netsh.exe' @('interface','ipv4','show','interfaces')
Native 'Firewall profiles' 'netsh.exe' @('advfirewall','show','allprofiles')
Inspect 'Offline network services' { foreach ($Service in @('Tcpip','Dhcp','Dnscache','NlaSvc','Netman','WlanSvc')) { Get-ItemProperty (Join-Path $SystemHiveRoot "$CurrentControlSetName\Services\$Service") -ErrorAction SilentlyContinue | Select @{N='Name';E={$Service}},Start,Type,ImagePath } }

Section '42 POWERSHELL AND RECOVERY TOOLS'
Inspect 'Recovery tool availability' {
    foreach ($Tool in @('bcdedit.exe','bcdboot.exe','bootrec.exe','bootsect.exe',
        'chkdsk.exe','dism.exe','sfc.exe','reagentc.exe','manage-bde.exe',
        'pnputil.exe','reg.exe')) {
        [pscustomobject]@{Tool=$Tool;Path=(Get-Command $Tool -ErrorAction SilentlyContinue).Source}
    }
}
Inspect 'WinRE environment variables' {
    Get-ChildItem Env: -ErrorAction SilentlyContinue |
        Where-Object Name -Match '^(SystemDrive|SystemRoot|TEMP|TMP|ProgramData|windir)$' |
        Sort Name
}
Native 'Recovery environment version' 'ver.exe' @()
Native 'Recovery environment architecture' 'wmic.exe' @('os','get','OSArchitecture','/value')

Section '43 SYSTEM CONFIGURATION'
Inspect 'Control set metadata' { Get-ItemProperty (Join-Path $SystemHiveRoot 'Select') -ErrorAction SilentlyContinue | Select Current,Default,Failed,LastKnownGood }
Inspect 'Session manager configuration' { Get-ItemProperty (Join-Path $SystemHiveRoot "$CurrentControlSetName\Control\Session Manager") -ErrorAction SilentlyContinue | Select BootExecute,PendingFileRenameOperations,SetupExecute,Execute }
Inspect 'Memory management configuration' { Get-ItemProperty (Join-Path $SystemHiveRoot "$CurrentControlSetName\Control\Session Manager\Memory Management") -ErrorAction SilentlyContinue | Select ClearPageFileAtShutdown,ExistingPageFiles,PagingFiles }
Inspect 'Power configuration' { Get-ItemProperty (Join-Path $SystemHiveRoot "$CurrentControlSetName\Control\Power") -ErrorAction SilentlyContinue | Select HibernateEnabled,HiberbootEnabled }

Section '44 OFFLINE WINDOWS INVENTORY'
Inspect 'Windows version metadata' { Get-ItemProperty (Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion') -ErrorAction SilentlyContinue | Select ProductName,DisplayVersion,CurrentBuild,CurrentBuildNumber,UBR,InstallationType,InstallDate }
Inspect 'Windows system architecture' { Get-Item (Join-Path $WindowsRoot 'System32\ntoskrnl.exe') -ErrorAction SilentlyContinue | Select FullName,Length,VersionInfo }
Inspect 'Installed update packages' { Get-ChildItem (Join-Path $WindowsRoot 'servicing\Packages') -Filter '*.mum' -ErrorAction SilentlyContinue | Sort LastWriteTime -Descending | Select -First 200 Name,LastWriteTime }
Inspect 'Pending reboot indicators' {
    [pscustomobject]@{
        PendingXml = Test-Path (Join-Path $WindowsRoot 'WinSxS\pending.xml')
        RebootPending = Test-Path (Join-Path $SoftwareHiveRoot 'Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending')
        UpdateExeVolatile = (Get-ItemProperty (Join-Path $SoftwareHiveRoot 'Microsoft\Updates') -Name UpdateExeVolatile -ErrorAction SilentlyContinue).UpdateExeVolatile
    }
}

Section '45 FINAL EVIDENCE'
Inspect 'Recovery backup contents' { Get-ChildItem $Backup -Recurse -Force -ErrorAction SilentlyContinue | Sort Length -Descending | Select -First 100 FullName,Length,LastWriteTime }
Inspect 'Critical target files final state' {
    foreach ($Path in @(
        (Join-Path $WindowsRoot 'System32\config\SYSTEM'),
        (Join-Path $WindowsRoot 'System32\config\SOFTWARE'),
        (Join-Path $WindowsRoot 'System32\ntoskrnl.exe'),
        (Join-Path $WindowsRoot 'System32\winload.efi')) {
        Get-Item $Path -Force -ErrorAction SilentlyContinue |
            Select FullName,Length,LastWriteTime,Attributes
    }
}
Inspect 'Final target volume state' { Get-Volume -DriveLetter $WindowsDrive.TrimEnd(':') -ErrorAction SilentlyContinue | Select DriveLetter,FileSystem,HealthStatus,OperationalStatus,Size,SizeRemaining }
Write-Output "WinRE extended diagnostics completed for $WindowsDrive."

Section '31 UNLOCK AND VERIFICATION'
Write-Output 'Order: backup -> diagnosis -> supported policy fixes -> DISM -> SFC -> final verification.'
Write-Output "BackupReady: $BackupReady"

)PS" LR"PS(
Section 'SAFE REPAIRS'
if (!$DiagnosticOnly -and $IsWinRE -and $OfflineBackupReady -and
    $LoadedRecoveryHives -contains 'SYSIM_RECOVERY_SOFTWARE') {
    $OfflinePolicyRepairFailed = $false
    $script:RepairFailure = $false
    $OfflinePolicyPrefix = 'HKLM:\Software\'
    foreach ($Fix in $PolicyFixes) {
        if ($Fix.Root -ne 'HKLM') { continue }
        if (!$Fix.Path.StartsWith($OfflinePolicyPrefix,[StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $RelativePolicyPath = $Fix.Path.Substring($OfflinePolicyPrefix.Length)
        $OfflinePolicyPath = Join-Path $SoftwareHiveRoot $RelativePolicyPath
        try {
            $Current = Get-ItemProperty -LiteralPath $OfflinePolicyPath -Name $Fix.Name -ErrorAction SilentlyContinue
            if ($null -ne $Current) {
                if ($Fix.Delete) { Remove-ItemProperty -LiteralPath $OfflinePolicyPath -Name $Fix.Name -ErrorAction Stop }
                else { New-ItemProperty -LiteralPath $OfflinePolicyPath -Name $Fix.Name -Value ([int]$Fix.Value) -PropertyType DWord -Force -ErrorAction Stop | Out-Null }
                Write-Output "OFFLINE POLICY RESTORED: $OfflinePolicyPath\$($Fix.Name)"
            }
        } catch {
            $OfflinePolicyRepairFailed = $true
            Write-Output "OFFLINE POLICY ERROR: $OfflinePolicyPath\$($Fix.Name): $_"
        }
    }
    $OfflineExplorerPolicy = Join-Path $SoftwareHiveRoot 'Microsoft\Windows\CurrentVersion\Policies\Explorer'
    UnlockRunRestrictions $OfflineExplorerPolicy 'selected offline SOFTWARE'
    $OfflineIfeoPath = Join-Path $SoftwareHiveRoot 'Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
    UnlockIfeoDebuggers $OfflineIfeoPath 'selected offline SOFTWARE'
    UnlockPolicyLockTrees $SoftwareHiveRoot 'selected offline SOFTWARE'
    $OfflineSystemSoftwareFailed = $OfflinePolicyRepairFailed -or $script:RepairFailure

    foreach ($UserHive in $LoadedUserHives) {
        $UserRegistryRoot = "Registry::HKEY_USERS\$($UserHive.Mount)"
        $UserPolicyFailed = $false
        $script:RepairFailure = $false
        foreach ($Fix in $PolicyFixes) {
            if ($Fix.Root -ne 'HKCU' -or !$Fix.Path.StartsWith('HKCU:\',[StringComparison]::OrdinalIgnoreCase)) { continue }
            $RelativeUserPolicyPath = $Fix.Path.Substring(6)
            $OfflineUserPolicyPath = Join-Path $UserRegistryRoot $RelativeUserPolicyPath
            try {
                $Current = Get-ItemProperty -LiteralPath $OfflineUserPolicyPath -Name $Fix.Name -ErrorAction SilentlyContinue
                if ($null -ne $Current) {
                    if ($Fix.Delete) { Remove-ItemProperty -LiteralPath $OfflineUserPolicyPath -Name $Fix.Name -ErrorAction Stop }
                    else { New-ItemProperty -LiteralPath $OfflineUserPolicyPath -Name $Fix.Name -Value ([int]$Fix.Value) -PropertyType DWord -Force -ErrorAction Stop | Out-Null }
                    Write-Output "USER POLICY RESTORED: $($UserHive.Profile)\$($Fix.Name)"
                }
            } catch {
                $UserPolicyFailed = $true
                Write-Output "USER POLICY ERROR: $($UserHive.Profile)\$($Fix.Name): $_"
            }
        }
        UnlockRunRestrictions (Join-Path $UserRegistryRoot 'Software\Microsoft\Windows\CurrentVersion\Policies\Explorer') $UserHive.Profile
        if ($script:RepairFailure) { $UserPolicyFailed = $true }
        if ($UserPolicyFailed) {
            Write-Output "User hive repair failed; restoring $($UserHive.Profile) from backup."
            & reg.exe unload "HKU\$($UserHive.Mount)" 2>&1 | ForEach-Object { Write-Output $_ }
            if ($LASTEXITCODE -eq 0) {
                try {
                    Copy-Item -LiteralPath $UserHive.Backup -Destination $UserHive.File -Force -ErrorAction Stop
                    $LoadedUserHives = @($LoadedUserHives | Where-Object { $_.Mount -ne $UserHive.Mount })
                    Write-Output "USER HIVE ROLLBACK COMPLETED: $($UserHive.Profile)"
                } catch { Write-Output "USER HIVE ROLLBACK FAILED: $($UserHive.Profile) : $_" }
            } else { Write-Output "Could not unload user hive; rollback skipped: $($UserHive.Profile)" }
        }
    }

    if ($OfflineSystemSoftwareFailed) {
        Write-Output 'Offline policy repair failed; restoring original SOFTWARE hive backup.'
        & reg.exe unload 'HKLM\SYSIM_RECOVERY_SOFTWARE' 2>&1 | ForEach-Object { Write-Output $_ }
        if ($LASTEXITCODE -eq 0) {
            try {
                Copy-Item -LiteralPath (Join-Path $Backup 'Offline_SOFTWARE.hiv') `
                    -Destination (Join-Path $WindowsRoot 'System32\config\SOFTWARE') -Force -ErrorAction Stop
                $LoadedRecoveryHives = @($LoadedRecoveryHives | Where-Object { $_ -ne 'SYSIM_RECOVERY_SOFTWARE' })
                Write-Output 'Offline SOFTWARE hive rollback completed.'
            } catch { Write-Output "Offline SOFTWARE rollback failed: $_" }
        } else { Write-Output 'Could not unload modified SOFTWARE hive; automatic rollback skipped.' }
    }
    if ($LoadedUserHives.Count -eq 0) { Write-Output 'No backed-up offline user profiles were available for HKCU unlock.' }
}

if (!$DiagnosticOnly -and $IsWinRE -and $OfflineBackupReady -and $BootRepairNeeded) {
    if ($WindowsPartitionStyle -eq 'GPT' -and $EspDrive -and $BootBackupReady) {
        Write-Output 'Restoring UEFI boot files for the selected Windows installation.'
        $BootRepairOutput = & bcdboot.exe $WindowsRoot /s $EspDrive /f UEFI /v 2>&1
        $BootRepairCode = $LASTEXITCODE
        $BootRepairOutput | ForEach-Object { Write-Output $_ }
        $BootRepairVerified = $BootRepairCode -eq 0 -and
            (Test-Path -LiteralPath (Join-Path $EspRoot 'EFI\Microsoft\Boot\bootmgfw.efi')) -and
            (Test-Path -LiteralPath $BcdStore)
        if ($BootRepairVerified) {
            $BcdVerify = & bcdedit.exe /store $BcdStore /enum all 2>&1
            $BootRepairVerified = $LASTEXITCODE -eq 0 -and
                (($BcdVerify -join "`n") -match 'winload\.(efi|exe)')
        }
        if ($BootRepairVerified) {
            Write-Output 'UEFI boot repair verified.'
        } else {
            Write-Output 'UEFI boot repair verification failed; rolling back the saved EFI boot directory and BCD.'
            $SavedBootDirectory = Join-Path $Backup 'EFI-Microsoft-Boot'
            if (Test-Path -LiteralPath $SavedBootDirectory) {
                Remove-Item -LiteralPath $BootDirectory -Recurse -Force -ErrorAction SilentlyContinue
                New-Item -ItemType Directory -Path $BootDirectory -Force | Out-Null
                Get-ChildItem -LiteralPath $SavedBootDirectory -Force | ForEach-Object {
                    Copy-Item -LiteralPath $_.FullName -Destination $BootDirectory -Recurse -Force
                }
            }
            $SavedBcd = Join-Path $Backup 'BCD-ESP'
            if (Test-Path -LiteralPath $SavedBcd) {
                Copy-Item -LiteralPath $SavedBcd -Destination $BcdStore -Force
            } elseif (!$BcdExisted) {
                Remove-Item -LiteralPath $BcdStore -Force -ErrorAction SilentlyContinue
            }
            if (!$BootDirectoryExisted) {
                Remove-Item -LiteralPath $BootDirectory -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
)PS" LR"PS(
    } elseif ($WindowsPartitionStyle -eq 'MBR' -and $BootDrive -and $BootBackupReady) {
        Write-Output 'Restoring BIOS boot files for the selected Windows installation.'
        $BootRepairOutput = & bcdboot.exe $WindowsRoot /s $BootDrive /f BIOS /v 2>&1
        $BootRepairCode = $LASTEXITCODE
        $BootRepairOutput | ForEach-Object { Write-Output $_ }
        $BootRepairVerified = $BootRepairCode -eq 0 -and
            (Test-Path -LiteralPath (Join-Path $BootRoot 'bootmgr')) -and
            (Test-Path -LiteralPath $BcdStore)
        if ($BootRepairVerified) {
            $BcdVerify = & bcdedit.exe /store $BcdStore /enum all 2>&1
            $BootRepairVerified = $LASTEXITCODE -eq 0 -and
                (($BcdVerify -join "`n") -match 'winload\.(efi|exe)')
        }
        if ($BootRepairVerified) {
            Write-Output 'BIOS boot repair verified.'
        } else {
            Write-Output 'BIOS boot repair verification failed; restoring backed-up boot files.'
            $SavedBootDirectory = Join-Path $Backup 'BIOS-Boot'
            if (Test-Path -LiteralPath $SavedBootDirectory) {
                Remove-Item -LiteralPath $BootDirectory -Recurse -Force -ErrorAction SilentlyContinue
                New-Item -ItemType Directory -Path $BootDirectory -Force | Out-Null
                Get-ChildItem -LiteralPath $SavedBootDirectory -Force | ForEach-Object {
                    Copy-Item -LiteralPath $_.FullName -Destination $BootDirectory -Recurse -Force
                }
            } elseif (!(Test-Path -LiteralPath (Join-Path $Backup 'BIOS-Boot'))) {
                Remove-Item -LiteralPath $BootDirectory -Recurse -Force -ErrorAction SilentlyContinue
            }
            $SavedBcd = Join-Path $Backup 'BCD-BIOS'
            if (Test-Path -LiteralPath $SavedBcd) {
                Copy-Item -LiteralPath $SavedBcd -Destination $BcdStore -Force
            } elseif (!$BcdExisted) {
                Remove-Item -LiteralPath $BcdStore -Force -ErrorAction SilentlyContinue
            }
            $SavedBootManager = Join-Path $Backup 'bootmgr'
            if (Test-Path -LiteralPath $SavedBootManager) {
                Copy-Item -LiteralPath $SavedBootManager -Destination (Join-Path $BootRoot 'bootmgr') -Force
            } elseif (!$BootmgrExisted) {
                Remove-Item -LiteralPath (Join-Path $BootRoot 'bootmgr') -Force -ErrorAction SilentlyContinue
            }
        }
    } else {
        Write-Output 'Boot repair skipped: unique GPT ESP or active MBR partition and verified backup are required.'
    }
}

if (!$DiagnosticOnly -and !$IsWinRE -and $BackupReady) {
    foreach ($Fix in $PolicyFixes) {
        try {
            $Current = Get-ItemProperty -LiteralPath $Fix.Path -Name $Fix.Name -ErrorAction SilentlyContinue
            if ($null -ne $Current) {
                if ($Fix.Delete) { Remove-ItemProperty -LiteralPath $Fix.Path -Name $Fix.Name -ErrorAction Stop }
                else { New-ItemProperty -LiteralPath $Fix.Path -Name $Fix.Name -Value ([int]$Fix.Value) -PropertyType DWord -Force -ErrorAction Stop | Out-Null }
                Write-Output "POLICY RESTORED: $($Fix.Path)\$($Fix.Name)"
            }
        } catch { Write-Output "POLICY ERROR: $($Fix.Path)\$($Fix.Name): $_" }
    }

    $OnlineExplorerSubKey = 'Software\Microsoft\Windows\CurrentVersion\Policies\Explorer'
    foreach ($Hive in @('HKLM','HKCU')) {
        $RegistryPath = if ($Hive -eq 'HKLM') { "HKLM:\$OnlineExplorerSubKey" } else { "HKCU:\$OnlineExplorerSubKey" }
        $NativeRegistryPath = "$Hive\$OnlineExplorerSubKey"
        if (!(Test-Path -LiteralPath $RegistryPath)) { continue }
        $KeyBackup = Join-Path $Backup ("$Hive-Explorer-Restrictions.reg")
        $BackupOutput = & reg.exe export $NativeRegistryPath $KeyBackup /y 2>&1
        $KeyBackupReady = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $KeyBackup)
        $BackupOutput | ForEach-Object { Write-Output $_ }
        if (!$KeyBackupReady) {
            Write-Output "RUN POLICY CHANGE SKIPPED: could not back up $NativeRegistryPath"
            continue
        }
        $script:RepairFailure = $false
        UnlockRunRestrictions $RegistryPath $Hive
        if ($script:RepairFailure) {
            $RestoreOutput = & reg.exe import $KeyBackup 2>&1
            $RestoreOutput | ForEach-Object { Write-Output $_ }
            Write-Output "RUN POLICY rollback attempted for $Hive."
        }
    }

    $OnlineIfeoSubKey = 'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
    $OnlineIfeoPath = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
    $IfeoBackupPath = Join-Path $Backup 'HKLM-IFEO.reg'
    if (Test-Path -LiteralPath $OnlineIfeoPath) {
        $IfeoBackupOutput = & reg.exe export "HKLM\$OnlineIfeoSubKey" $IfeoBackupPath /y 2>&1
        $IfeoBackupReady = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $IfeoBackupPath)
        $IfeoBackupOutput | ForEach-Object { Write-Output $_ }
        if ($IfeoBackupReady) {
            $script:RepairFailure = $false
            UnlockIfeoDebuggers $OnlineIfeoPath 'online HKLM'
            if ($script:RepairFailure) {
                $RestoreOutput = & reg.exe import $IfeoBackupPath 2>&1
                $RestoreOutput | ForEach-Object { Write-Output $_ }
                Write-Output 'IFEO rollback attempted.'
            }
        } else { Write-Output 'IFEO changes skipped because backup failed.' }
    }

    $OnlinePolicyBackups = @()
    $OnlinePolicyBackupsReady = $true
    foreach ($RelativePath in @('Policies\Microsoft\Windows\Safer','Policies\Microsoft\Windows\SrpV2')) {
        $PolicyPath = Join-Path 'HKLM:\SOFTWARE' $RelativePath
        if (!(Test-Path -LiteralPath $PolicyPath)) { continue }
        $NativePath = "HKLM\SOFTWARE\$RelativePath"
        $PolicyBackup = Join-Path $Backup ((($RelativePath -replace '[\\]','_')) + '.reg')
        $PolicyBackupOutput = & reg.exe export $NativePath $PolicyBackup /y 2>&1
        $PolicyBackupOutput | ForEach-Object { Write-Output $_ }
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $PolicyBackup)) {
            $OnlinePolicyBackups += $PolicyBackup
        } else { $OnlinePolicyBackupsReady = $false }
    }
    if ($OnlinePolicyBackupsReady) {
        $script:RepairFailure = $false
        UnlockPolicyLockTrees 'HKLM:\SOFTWARE' 'online HKLM'
        if ($script:RepairFailure) {
            foreach ($PolicyBackup in $OnlinePolicyBackups) {
                & reg.exe import $PolicyBackup 2>&1 | ForEach-Object { Write-Output $_ }
            }
            Write-Output 'SRP/AppLocker policy rollback attempted.'
        }
    } else { Write-Output 'SRP/AppLocker cleanup skipped because backup failed.' }
} elseif ($IsWinRE -or $DiagnosticOnly) {
    Write-Output 'Online registry policies were not changed from WinRE.'
} else { Write-Output 'Policy changes skipped because at least one registry hive backup failed.' }

)PS" LR"PS(
if ($DiagnosticOnly) {
    Write-Output 'Diagnostic-only mode: no repair commands were run.'
} elseif (!$IsWinRE) {
    Native 'DISM RestoreHealth' 'dism.exe' @('/Online','/Cleanup-Image','/RestoreHealth')
    Native 'SFC repair pass 1' 'sfc.exe' @('/scannow')
    Native 'SFC repair pass 2' 'sfc.exe' @('/scannow')
} elseif ($OfflineBackupReady) {
    $OfflineScratch = Join-Path $Backup 'Scratch'
    $OfflineScratchReady = $false
    try {
        New-Item -ItemType Directory -Path $OfflineScratch -Force -ErrorAction Stop | Out-Null
        $OfflineScratchReady = $true
    } catch { Write-Output "Offline scratch directory creation failed: $_" }
    if ($OfflineScratchReady) {
        Native 'Offline DISM RestoreHealth' 'dism.exe' @("/Image:$WindowsDrive\",'/Cleanup-Image','/RestoreHealth',"/ScratchDir:$OfflineScratch","/LogPath:$(Join-Path $OfflineScratch 'DISM.log')")
    } else { Write-Output 'Offline DISM repair skipped: no writable scratch directory.' }
    Native 'Offline SFC repair pass 1' 'sfc.exe' @('/scannow',"/offbootdir=$WindowsDrive\","/offwindir=$WindowsRoot")
    Native 'Offline SFC repair pass 2' 'sfc.exe' @('/scannow',"/offbootdir=$WindowsDrive\","/offwindir=$WindowsRoot")
} else {
    Write-Output 'Offline repairs skipped because target selection or hive backup was not unambiguous.'
}

Section '32 FINAL BOOT / SYSTEM TEST'
if ($IsWinRE -and $BcdStore -and (Test-Path -LiteralPath $BcdStore)) {
    Native 'Final selected Windows BCD check' 'bcdedit.exe' @('/store',$BcdStore,'/enum','{bootmgr}')
} elseif (!$IsWinRE) { Native 'Final BCD check' 'bcdedit.exe' @('/enum','{bootmgr}') }
if (!$IsWinRE) {
    Native 'Final DISM health check' 'dism.exe' @('/Online','/Cleanup-Image','/CheckHealth')
    Native 'Final protected file verification' 'sfc.exe' @('/verifyonly')
} elseif ($OfflineBackupReady) {
    $OfflineScratch = Join-Path $Backup 'Scratch'
    Native 'Final offline DISM check' 'dism.exe' @("/Image:$WindowsDrive\",'/Cleanup-Image','/CheckHealth',"/ScratchDir:$OfflineScratch","/LogPath:$(Join-Path $OfflineScratch 'DISM-final.log')")
    Native 'Final offline SFC verification' 'sfc.exe' @('/verifyonly',"/offbootdir=$WindowsDrive\","/offwindir=$WindowsRoot")
}
if ($IsWinRE -and $OfflineBackupReady) {
    $ServicesPath = Join-Path $SystemHiveRoot "$CurrentControlSetName\Services"
    foreach ($Service in @('RpcSs','DcomLaunch','PlugPlay','EventLog')) {
        Inspect "Selected Windows critical service $Service" { Get-ItemProperty (Join-Path $ServicesPath $Service) -ErrorAction SilentlyContinue | Select Start,Type,ErrorControl,ImagePath }
    }
} else {
    Inspect 'Critical services after repair' { Get-Service RpcSs,DcomLaunch,PlugPlay,EventLog -ErrorAction SilentlyContinue | Select Name,Status,StartType }
}
Write-Output "Registry backups: $Backup"
foreach ($MountName in $LoadedRecoveryHives) {
    & reg.exe unload "HKLM\$MountName" 2>&1 | ForEach-Object { Write-Output $_ }
}
foreach ($UserHive in $LoadedUserHives) {
    & reg.exe unload "HKU\$($UserHive.Mount)" 2>&1 | ForEach-Object { Write-Output $_ }
}
if ($EspTemporarilyMounted -and $WindowsDiskNumber -ge 0) {
    try {
        Set-Partition -DiskNumber $WindowsDiskNumber `
            -PartitionNumber $EspPartition.PartitionNumber `
            -RemoveDriveLetter -ErrorAction Stop
    } catch { Write-Output "Temporary ESP drive-letter cleanup failed: $_" }
}
if ($BootTemporarilyMounted -and $WindowsDiskNumber -ge 0) {
    try {
        Set-Partition -DiskNumber $WindowsDiskNumber `
            -PartitionNumber $BootPartitionNumber `
            -RemoveDriveLetter -ErrorAction Stop
    } catch { Write-Output "Temporary MBR system-partition drive-letter cleanup failed: $_" }
}
)PS";

static std::wstring EscapePowerShellLiteral(std::wstring value) {
    size_t pos = 0;
    while ((pos = value.find(L'\'', pos)) != std::wstring::npos) {
        value.insert(pos, 1, L'\'');
        pos += 2;
    }
    return value;
}

struct RecoveryScriptLease {
    HANDLE process;
    HANDLE script;
    wchar_t scriptPath[MAX_PATH];
    wchar_t workingDirectory[MAX_PATH];
};

static void CleanupRecoveryWorkspace(const std::wstring& scriptPath,
    const std::wstring& workingDirectory) {
    if (!scriptPath.empty()) DeleteFileW(scriptPath.c_str());
    if (!workingDirectory.empty()) RemoveDirectoryW(workingDirectory.c_str());
}

static unsigned __stdcall ReleaseRecoveryScriptLease(void* parameter) {
    auto* lease = static_cast<RecoveryScriptLease*>(parameter);
    WaitForSingleObject(lease->process, INFINITE);
    CloseHandle(lease->process);
    CloseHandle(lease->script);
    CleanupRecoveryWorkspace(lease->scriptPath, lease->workingDirectory);
    HeapFree(GetProcessHeap(), 0, lease);
    return 0;
}

static bool WriteRecoveryScript(
    const std::wstring& path,
    const std::wstring& backupRoot,
    bool inRecoveryEnvironment,
    bool diagnosticOnly,
    std::wstring& script,
    HANDLE& protectedScript
) {
    protectedScript = nullptr;
    script.insert(0, std::wstring(L"$SYSIM_IsRecoveryEnvironment = $") +
        (inRecoveryEnvironment ? L"true" : L"false") + L"\r\n");
    script.insert(0, std::wstring(L"$SYSIM_DiagnosticOnly = $") +
        (diagnosticOnly ? L"true" : L"false") + L"\r\n");
    script.insert(0, L"$RecoveryBackupRoot = '" + EscapePowerShellLiteral(backupRoot) + L"'\r\n");
    std::wstring selectedDrive = EscapePowerShellLiteral(UnlockTools::GetOfflineDriveHint());
    script.insert(0, L"$SelectedWindowsDrive = '" + selectedDrive + L"'\r\n");
    std::wstring fixes = L"$PolicyFixes = @(\r\n";
    bool first = true;
    for (const auto& restriction : UnlockTools::GetKnownRestrictions()) {
        for (HKEY hive : restriction.hives) {
            const wchar_t* hiveName = hive == HKEY_CURRENT_USER ? L"HKCU" :
                (hive == HKEY_LOCAL_MACHINE ? L"HKLM" : nullptr);
            if (!hiveName) continue;
            if (!first) fixes += L",\r\n";
            first = false;
            fixes += L"@{ Root = '" + std::wstring(hiveName) + L"'; Path = '" +
                std::wstring(hiveName) + L":\\" +
                EscapePowerShellLiteral(restriction.subKey) + L"'; Name = '" +
                EscapePowerShellLiteral(restriction.valueName) + L"'; Value = " +
                std::to_wstring(restriction.disableValue) + L"; Delete = $" +
                (restriction.deleteInsteadOfSet ? L"true" : L"false") + L" }";
        }
    }
    fixes += L"\r\n)\r\n";
    script.insert(0, fixes);

    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, script.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 1) return false;
    std::vector<char> utf8(static_cast<size_t>(utf8Size));
    if (!WideCharToMultiByte(CP_UTF8, 0, script.c_str(), -1,
        utf8.data(), utf8Size, nullptr, nullptr)) return false;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const BYTE bom[] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    bool ok = WriteFile(file, bom, sizeof(bom), &written, nullptr) &&
        written == sizeof(bom) &&
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8Size - 1), &written, nullptr) &&
        written == static_cast<DWORD>(utf8Size - 1);
    ok = ok && FlushFileBuffers(file);
    if (!ok) {
        CloseHandle(file);
        return false;
    }
    protectedScript = file;
    return true;
}

static bool CreateRecoveryWorkspace(std::wstring& directory) {
    wchar_t tempPath[MAX_PATH] = {};
    const DWORD tempPathLength = GetTempPathW(MAX_PATH, tempPath);
    if (tempPathLength == 0 || tempPathLength >= MAX_PATH) return false;

    wchar_t uniquePath[MAX_PATH] = {};
    if (!GetTempFileNameW(tempPath, L"SIM", 0, uniquePath)) return false;
    if (!DeleteFileW(uniquePath)) return false;
    if (!CreateDirectoryW(uniquePath, nullptr)) return false;
    directory = uniquePath;
    return true;
}

static bool EnableRecoveryBackupPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool lookedUp = LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME,
        &privileges.Privileges[0].Luid) != FALSE;
    BOOL adjusted = FALSE;
    if (lookedUp) {
        SetLastError(ERROR_SUCCESS);
        adjusted = AdjustTokenPrivileges(token, FALSE, &privileges,
            sizeof(privileges), nullptr, nullptr);
    }
    const DWORD error = GetLastError();
    CloseHandle(token);
    return lookedUp && adjusted && error == ERROR_SUCCESS;
}

static DWORD SaveMountedHiveBackup(const wchar_t* mountName,
    const std::wstring& destination) {
    HKEY hive = nullptr;
    LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, mountName, 0,
        KEY_READ | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &hive);
    if (status != ERROR_SUCCESS) return static_cast<DWORD>(status);

    if (!EnableRecoveryBackupPrivilege()) {
        status = static_cast<LONG>(GetLastError());
        if (status == ERROR_SUCCESS) status = ERROR_PRIVILEGE_NOT_HELD;
    } else {
        status = RegSaveKeyW(hive, destination.c_str(), nullptr);
    }
    RegCloseKey(hive);
    return static_cast<DWORD>(status);
}

static bool CreateOfflineBackupDirectory(const std::wstring& selectedDrive,
    const std::wstring& backupPath, bool backupUsers = true,
    bool backupLogonFiles = true, bool* userBackupsComplete = nullptr,
    std::wstring* failure = nullptr, bool* systemHiveBackupComplete = nullptr) {

    if (selectedDrive.size() >= 2 &&
        towupper(selectedDrive[0]) == L'X' && selectedDrive[1] == L':') {
        if (failure) *failure += L"Отказ: целевой диск X: (WinRE) не может быть изменён.\r\n";
        if (userBackupsComplete) *userBackupsComplete = false;
        if (systemHiveBackupComplete) *systemHiveBackupComplete = false;
        return false;
    }

    if (userBackupsComplete) *userBackupsComplete = true;
    if (systemHiveBackupComplete) *systemHiveBackupComplete = true;
    auto setFailure = [&](const std::wstring& path) {
        const DWORD errorCode = GetLastError();
        if (failure) {
            *failure += L"Не удалось скопировать " + path + L" (код " +
                std::to_wstring(errorCode) + L").\r\n";
        }
    };

    const std::wstring backupBase = selectedDrive + L"SYSIM_RecoveryBackup";
    if (!CreateDirectoryW(backupBase.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        setFailure(backupBase);
        return false;
    }
    if (!CreateDirectoryW(backupPath.c_str(), nullptr)) {
        setFailure(backupPath);
        return false;
    }
    const std::wstring usersBackup = backupPath + L"\\Users";
    const std::wstring system32Backup = backupPath + L"\\System32";
    if (backupLogonFiles && !CreateDirectoryW(system32Backup.c_str(), nullptr)) {
        setFailure(system32Backup);
        return false;
    }

    const wchar_t* hives[] = { L"SYSTEM", L"SOFTWARE" };
    for (const wchar_t* hive : hives) {
        const std::wstring source = selectedDrive + L"Windows\\System32\\config\\" + hive;
        const std::wstring destination = backupPath + L"\\" + hive + L".hiv";
        if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            setFailure(source);
            return false;
        }
        if (!CopyFileW(source.c_str(), destination.c_str(), TRUE)) {
            DWORD copyError = GetLastError();
            if (copyError == ERROR_SHARING_VIOLATION) {
                const wchar_t* mountedHive = _wcsicmp(hive, L"SYSTEM") == 0
                    ? L"OfflineSystem" : L"OfflineSoftware";
                const DWORD saveError = SaveMountedHiveBackup(mountedHive, destination);
                if (saveError == ERROR_SUCCESS) continue;
                SetLastError(saveError);
            } else {
                SetLastError(copyError);
            }
            if (_wcsicmp(hive, L"SYSTEM") == 0) {
                if (systemHiveBackupComplete) *systemHiveBackupComplete = false;
                setFailure(source);
                continue;
            }
            setFailure(source);
            return false;
        }
    }

    if (backupLogonFiles) {
        for (const wchar_t* fileName : { L"utilman.exe", L"sethc.exe" }) {
            const std::wstring source = selectedDrive + L"Windows\\System32\\" + fileName;
            if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if (!CopyFileW(source.c_str(), (system32Backup + L"\\" + fileName).c_str(), TRUE)) {
                setFailure(source);
                return false;
            }
        }
    }

    if (backupUsers) {
        if (!CreateDirectoryW(usersBackup.c_str(), nullptr)) {
            if (userBackupsComplete) *userBackupsComplete = false;
            setFailure(usersBackup);
        } else {
            const std::wstring usersRoot = selectedDrive + L"Users\\*";
            WIN32_FIND_DATAW entry{};
            HANDLE find = FindFirstFileW(usersRoot.c_str(), &entry);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                        wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0 ||
                        wcscmp(entry.cFileName, L"Public") == 0 || wcscmp(entry.cFileName, L"Default") == 0 ||
                        wcscmp(entry.cFileName, L"Default User") == 0 || wcscmp(entry.cFileName, L"All Users") == 0)
                        continue;
                    const std::wstring source = selectedDrive + L"Users\\" + entry.cFileName + L"\\NTUSER.DAT";
                    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                    const std::wstring destination = usersBackup + L"\\" + entry.cFileName + L".NTUSER.DAT";
                    if (!CopyFileW(source.c_str(), destination.c_str(), TRUE)) {
                        if (userBackupsComplete) *userBackupsComplete = false;
                        setFailure(source);
                    }
                } while (FindNextFileW(find, &entry));
                FindClose(find);
            } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
                if (userBackupsComplete) *userBackupsComplete = false;
                setFailure(usersRoot);
            }
        }
    }

    const std::wstring hosts = selectedDrive + L"Windows\\System32\\drivers\\etc\\hosts";
    if (GetFileAttributesW(hosts.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !CopyFileW(hosts.c_str(), (backupPath + L"\\hosts").c_str(), TRUE)) {
        setFailure(hosts);
        return false;
    }
    return true;
}

void RunFullRecovery(bool diagnosticOnly) {
    HWND owner = App::Instance() ? App::Instance()->GetHWND() : nullptr;
    const bool inRecoveryEnvironment = UnlockTools::IsRecoveryEnvironment();
    
    std::wstring selectedDrive;
    if (inRecoveryEnvironment) {
        selectedDrive = UnlockTools::GetOfflineDriveHint();
        if (selectedDrive.empty()) {
            auto installations = UnlockTools::ScanDrivesWithWindows();
            if (installations.size() == 1) {
                selectedDrive = installations.front();
                UnlockTools::SetOfflineDriveHint(selectedDrive);
            } else if (installations.empty()) {
                MessageBoxW(owner, L"Не найдено ни одной установки Windows на дисках. Разблокировка невозможна.",
                            L"Ошибка", MB_OK | MB_ICONERROR);
                return;
            } else {
                 MessageBoxW(owner, L"Найдено несколько установок Windows. Выберите целевой диск в настройках.",
                             L"Ошибка", MB_OK | MB_ICONERROR);
                 return;
            }
        }
        
        if (_wcsicmp(selectedDrive.c_str(), L"X:\\") == 0 || _wcsicmp(selectedDrive.c_str(), L"X:") == 0) {
             MessageBoxW(owner, L"Нельзя разблокировать среду восстановления (X:). Выберите диск с Windows (C:, D: и т.д.).",
                         L"Ошибка", MB_OK | MB_ICONERROR);
             return;
        }
        
        if (!selectedDrive.empty() && selectedDrive.back() != L'\\') {
            selectedDrive += L"\\";
        }
        }
    // }

    bool winPeTargetMissing = false;
    if (inRecoveryEnvironment) {
         std::wstring checkPath = selectedDrive + L"Windows\\System32\\ntoskrnl.exe";
         if (GetFileAttributesW(checkPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
             winPeTargetMissing = true;
         }
    }

    std::wstring backupRootBase;
    if (inRecoveryEnvironment) {
        if (!selectedDrive.empty() && !winPeTargetMissing) {
            backupRootBase = selectedDrive + L"SYSIM_RecoveryBackup";
        }
    }
    else {
        wchar_t programData[MAX_PATH] = {};
        const DWORD programDataLength = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
        if (programDataLength > 0 && programDataLength < MAX_PATH) {
            backupRootBase = std::wstring(programData) + L"\\SYSIM\\RecoveryBackup";
        }
    }

    if (inRecoveryEnvironment && backupRootBase.empty()) {
         MessageBoxW(owner, L"Не удалось определить путь для резервных копий. Проверьте выбор диска.",
                     L"Ошибка", MB_OK | MB_ICONERROR);
         return;
    }

    std::wstring backupRoot;
    if (!backupRootBase.empty()) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t backupName[64]{};
        swprintf_s(backupName, L"Backup_%04u%02u%02u_%02u%02u%02u",
                   time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        backupRoot = backupRootBase + L"\\" + backupName + L"_" +
                     std::to_wstring(GetTickCount64());
    }

        std::wstring confirmation = diagnosticOnly
                ? L"Полная диагностика проверит более 90 параметров диска, загрузки, служб, политик, профилей и BSOD. "
                    L"Изменения Windows и ремонт DISM/SFC не выполняются. Создадутся только резервные копии hive.\n\n"
                : L"Экстренная разблокировка сохранит копии системных hive и BCD, проверит 32 области Windows, "
                    L"снимет обнаруженные поддерживаемые ограничения и запустит DISM/SFC.\n\n";
    if (winPeTargetMissing) {
        confirmation += L"Выбранный диск не содержит доступную установку Windows или диск не выбран. "
            L"Проверьте букву диска в настройках. Будет выполнена только диагностика дисков/BitLocker; ремонт пропускается.\n\n";
    }
    confirmation += L"Ремонт EFI, драйверов и служб без точного диагноза выполняться не будет.\n\n"
        L"Продолжить с правами администратора?";
    if (MessageBoxW(owner, confirmation.c_str(), L"Экстренная разблокировка",
        MB_YESNO | MB_ICONWARNING) != IDYES) return;

    std::wstring runFolder;
    if (!CreateRecoveryWorkspace(runFolder)) {
        MessageBoxW(owner, L"Не удалось создать временную рабочую папку для сценария восстановления.",
            L"Экстренная разблокировка", MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring scriptPath = runFolder + L"\\Unlock.ps1";
    std::wstring script(RECOVERY_SCRIPT);
    HANDLE protectedScript = nullptr;
    if (!WriteRecoveryScript(scriptPath, backupRoot, inRecoveryEnvironment, diagnosticOnly,
        script, protectedScript)) {
        CleanupRecoveryWorkspace(scriptPath, runFolder);
        MessageBoxW(owner, L"Не удалось записать сценарий диагностики.",
            L"Экстренная разблокировка", MB_OK | MB_ICONERROR);
        return;
    }

    wchar_t systemDirectory[MAX_PATH] = {};
    UINT systemDirectoryLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (systemDirectoryLength == 0 || systemDirectoryLength >= MAX_PATH) {
        CloseHandle(protectedScript);
        CleanupRecoveryWorkspace(scriptPath, runFolder);
        MessageBoxW(owner, L"Не удалось определить каталог PowerShell в текущей среде.",
            L"Экстренная разблокировка", MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring powershellPath = std::wstring(systemDirectory) +
        L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (GetFileAttributesW(powershellPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t discoveredPath[MAX_PATH] = {};
        const DWORD discoveredLength = SearchPathW(nullptr, L"powershell.exe", nullptr,
            MAX_PATH, discoveredPath, nullptr);
        if (discoveredLength == 0 || discoveredLength >= MAX_PATH) {
            CloseHandle(protectedScript);
            CleanupRecoveryWorkspace(scriptPath, runFolder);
            if (inRecoveryEnvironment) {
                const std::wstring selectedDrive = UnlockTools::GetOfflineDriveHint();
                const auto installations = UnlockTools::ScanDrivesWithWindows();
                const bool selectedTargetFound = !selectedDrive.empty() &&
                    std::any_of(installations.begin(), installations.end(),
                        [&](const std::wstring& drive) {
                            return _wcsicmp(drive.c_str(), selectedDrive.c_str()) == 0;
                        });
                if (winPeTargetMissing || !selectedTargetFound || backupRoot.empty()) {
                    MessageBoxW(owner,
                        L"В WinRE нет PowerShell, а выбранный Windows-диск недоступен. "
                        L"Восстановление остановлено; раздел X: не изменялся.",
                        L"Экстренная разблокировка", MB_OK | MB_ICONWARNING);
                    return;
                }
                bool userBackupsComplete = true;
                bool systemHiveBackupComplete = true;
                std::wstring backupFailure;
                if (!CreateOfflineBackupDirectory(selectedDrive, backupRoot, false, false,
                    &userBackupsComplete, &backupFailure, &systemHiveBackupComplete)) {
                    MessageBoxW(owner,
                        (L"Не удалось сохранить обязательную копию SYSTEM/SOFTWARE/hosts. "
                            L"Офлайн-изменения отменены.\r\n" + backupFailure).c_str(),
                        L"Экстренная разблокировка", MB_OK | MB_ICONERROR);
                    return;
                }
                                const std::wstring report = UnlockTools::GetBestUnlockReport(!diagnosticOnly,
                                        userBackupsComplete, systemHiveBackupComplete);
                                g_lastReport = diagnosticOnly
                                        ? L"Полная диагностика PowerShell недоступна; выполнено только нативное сканирование политик. "
                                            L"Изменений Windows не вносилось.\r\n"
                                        : L"PowerShell в WinRE отсутствует; выполнен только нативный офлайн-ремонт политик. "
                                            L"DISM/SFC и расширенная диагностика не запускались.\r\n";
                                g_lastReport += L"Резервная копия: " + backupRoot + L"\r\n" + backupFailure + L"\r\n" + report;
                MessageBoxW(owner, g_lastReport.c_str(), L"Экстренная разблокировка",
                    MB_OK | MB_ICONINFORMATION);
                return;
            }
            MessageBoxW(owner,
                L"В этой среде не найден Windows PowerShell. Запустите восстановление в WinRE-образе с поддержкой PowerShell.",
                L"Экстренная разблокировка", MB_OK | MB_ICONERROR);
            return;
        }
        powershellPath = discoveredPath;
    }
    std::wstring parameters = L"-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + L"\"";

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executeInfo.hwnd = owner;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = powershellPath.c_str();
    executeInfo.lpParameters = parameters.c_str();
    executeInfo.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&executeInfo)) {
        DWORD error = GetLastError();
        CloseHandle(protectedScript);
        CleanupRecoveryWorkspace(scriptPath, runFolder);
        wchar_t message[256] = {};
        swprintf_s(message, L"Не удалось запустить PowerShell с повышенными правами (код %lu).", error);
        MessageBoxW(owner, message, L"Экстренная разблокировка", MB_OK | MB_ICONERROR);
        return;
    }
    auto* lease = static_cast<RecoveryScriptLease*>(HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RecoveryScriptLease)));
    if (lease && executeInfo.hProcess) {
        lease->process = executeInfo.hProcess;
        lease->script = protectedScript;
        wcscpy_s(lease->scriptPath, scriptPath.c_str());
        wcscpy_s(lease->workingDirectory, runFolder.c_str());
        uintptr_t thread = _beginthreadex(nullptr, 0,
            ReleaseRecoveryScriptLease, lease, 0, nullptr);
        if (thread) CloseHandle(reinterpret_cast<HANDLE>(thread));
        else {
            WaitForSingleObject(lease->process, INFINITE);
            CloseHandle(lease->process);
            CloseHandle(lease->script);
            CleanupRecoveryWorkspace(lease->scriptPath, lease->workingDirectory);
            HeapFree(GetProcessHeap(), 0, lease);
        }
    } else {
        if (lease) HeapFree(GetProcessHeap(), 0, lease);
        if (executeInfo.hProcess) {
            WaitForSingleObject(executeInfo.hProcess, INFINITE);
            CloseHandle(executeInfo.hProcess);
        }
        CloseHandle(protectedScript);
        CleanupRecoveryWorkspace(scriptPath, runFolder);
    }

    g_lastReport = L"Экстренная разблокировка запущена.\r\n\r\nРезервные копии:\r\n" +
        (backupRoot.empty() ? L"не создавались (целевая установка не выбрана)" : backupRoot) +
        L"\r\n\r\nВременный сценарий будет удалён после завершения.";
    MessageBoxW(owner, g_lastReport.c_str(), L"Экстренная разблокировка",
        MB_OK | MB_ICONINFORMATION);
}

void RunWinPeFullDiagnosis() {
    RunFullRecovery(true);
}

// Вспомогательные функции
static bool HitTestRect(const RectF& rect, float x, float y) {
    return x >= rect.X && x < rect.X + rect.Width &&
        y >= rect.Y && y < rect.Y + rect.Height;
}

static void DrawCheckbox(
    Graphics& g, const RectF& boxRect, bool checked, const wchar_t* label,
    Font& font, SolidBrush& textBrush, SolidBrush& controlBg,
    Pen& borderPen, SolidBrush& checkBrush
) {
    g.FillRectangle(&controlBg, boxRect);
    g.DrawRectangle(&borderPen, boxRect);
    if (checked) {
        g.FillRectangle(&checkBrush, RectF(
            boxRect.X + 4.0f, boxRect.Y + 4.0f,
            boxRect.Width - 8.0f, boxRect.Height - 8.0f));
    }
    StringFormat leftFormat;
    leftFormat.SetAlignment(StringAlignmentNear);
    leftFormat.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(label, -1, &font,
        RectF(boxRect.X + 24.0f, boxRect.Y - 4.0f, 120.0f, 24.0f),
        &leftFormat, &textBrush);
}

static void DrawButton(
    Graphics& g, const RectF& r, const wchar_t* label,
    Font& font, SolidBrush& textBrush, SolidBrush& controlBg, Pen& borderPen
) {
    g.FillRectangle(&controlBg, r);
    g.DrawRectangle(&borderPen, r);
    StringFormat cf;
    cf.SetAlignment(StringAlignmentCenter);
    cf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(label, -1, &font, r, &cf, &textBrush);
}

static void StartUnlockScan();

// Основная функция рисования
void DrawUnlockContent(Graphics& g, const RectF& contentArea, Font& contentFont) {
    (void)contentFont;

    if (g_lastReport.empty()) {
        g_lastReport = L"Сканирование блокировок...\r\n";
        StartUnlockScan();
    }

    float x = contentArea.X + 10.0f;
    float y = contentArea.Y + 8.0f;

    FontFamily ff(g_fontFamilyName.c_str());
    Font font(&ff, 11.0f, FontStyleRegular, UnitPixel);        // для чекбоксов
    Font smallFont(&ff, 10.5f, FontStyleRegular, UnitPixel);   // для кнопок
    SolidBrush textBrush(COLOR_TEXT);
    SolidBrush controlBg(COLOR_BUTTON_BG);
    SolidBrush checkBrush(COLOR_TEXT);
    Pen borderPen(COLOR_BORDER, 1.0f);

    const bool inRecovery = UnlockTools::IsRecoveryEnvironment();
    float reportY = inRecovery ? y + 8.0f : y + 28.0f;
    if (inRecovery) {
        g_checkboxRect = RectF();
        g_bcdCheckboxRect = RectF();
        g_aclCheckboxRect = RectF();
        g_bootCheckboxRect = RectF();
        g_ifeoCheckboxRect = RectF();
        g_buttonRect = RectF();
        g_ifeoScanRect = RectF(x, y, 148.0f, 24.0f);
        g_ifeoDeleteRect = RectF(x + 154.0f, y, 148.0f, 24.0f);
        g_refreshButtonRect = RectF(x + 308.0f, y, 92.0f, 24.0f);
        DrawButton(g, g_ifeoScanRect, L"Скан IFEO",
            smallFont, textBrush, controlBg, borderPen);
        DrawButton(g, g_ifeoDeleteRect, L"Удалить IFEO",
            smallFont, textBrush, controlBg, borderPen);
        DrawButton(g, g_refreshButtonRect, L"Обновить",
            smallFont, textBrush, controlBg, borderPen);
        reportY = y + 32.0f;
    }
    else {
        // Чекбоксы в одну строку
        const float checkboxW = 108.0f;
        const float checkboxGap = 8.0f;
        float startX = x;

        // Чекбокс 1: "Разблок."
        RectF boxRect1(startX, y, 16.0f, 16.0f);
        g_checkboxRect = RectF(startX - 4.0f, y - 4.0f, checkboxW, 24.0f);
        DrawCheckbox(g, boxRect1, g_immediateUnlock, L"Разблок.",
            font, textBrush, controlBg, borderPen, checkBrush);

        // Чекбокс 2: "BCD safeboot"
        startX += checkboxW + checkboxGap;
        RectF boxRect2(startX, y, 16.0f, 16.0f);
        g_bcdCheckboxRect = RectF(startX - 4.0f, y - 4.0f, checkboxW, 24.0f);
        DrawCheckbox(g, boxRect2, g_fixBcdSafeBoot, L"BCD safeboot",
            font, textBrush, controlBg, borderPen, checkBrush);

        // Чекбокс 3: "ACL"
        startX += checkboxW + checkboxGap;
        RectF boxRect3(startX, y, 16.0f, 16.0f);
        g_aclCheckboxRect = RectF(startX - 4.0f, y - 4.0f, checkboxW, 24.0f);
        DrawCheckbox(g, boxRect3, g_fixAcl, L"ACL",
            font, textBrush, controlBg, borderPen, checkBrush);

        // Чекбокс 4: "Boot"
        startX += checkboxW + checkboxGap;
        RectF boxRect4(startX, y, 16.0f, 16.0f);
        g_bootCheckboxRect = RectF(startX - 4.0f, y - 4.0f, checkboxW, 24.0f);
        DrawCheckbox(g, boxRect4, g_fixBoot, L"Boot",
            font, textBrush, controlBg, borderPen, checkBrush);

        // Чекбокс 5: "IFEO"
        startX += checkboxW + checkboxGap;
        RectF boxRect5(startX, y, 16.0f, 16.0f);
        g_ifeoCheckboxRect = RectF(startX - 4.0f, y - 4.0f, checkboxW, 24.0f);
        DrawCheckbox(g, boxRect5, g_fixIfeo, L"IFEO",
            font, textBrush, controlBg, borderPen, checkBrush);

        // Кнопки справа от чекбоксов
        float btnW = 80.0f;
        float btnH = 22.0f;
        float btnY = y + 1.0f;
        float btnX = contentArea.X + contentArea.Width - 10.0f - btnW - 4.0f - btnW;

        g_buttonRect = RectF(btnX, btnY, btnW, btnH);
        g_refreshButtonRect = RectF(btnX + btnW + 4.0f, btnY, btnW, btnH);

        DrawButton(g, g_buttonRect,
            g_immediateUnlock ? L"Выполнить" : L"Обновить",
            smallFont, textBrush, controlBg, borderPen);
        DrawButton(g, g_refreshButtonRect, L"Обновить",
            smallFont, textBrush, controlBg, borderPen);

        reportY = y + 28.0f;
    }

    // Область отчёта / лога
    float reportHeight = contentArea.Height - (reportY - contentArea.Y) - 8.0f;
    if (reportHeight > 10.0f) {
        std::vector<std::wstring> lines;
        std::wstringstream ss(g_lastReport);
        std::wstring line;
        while (std::getline(ss, line)) lines.push_back(line);

        float lineHeight = 16.0f;
        float totalTextHeight = (float)lines.size() * lineHeight;
        g_maxScroll[4] = (totalTextHeight > reportHeight)
            ? (int)(totalTextHeight - reportHeight) : 0;
        int offsetY = g_scrollOffset[4];

        SolidBrush reportBg(Color(45, 45, 45));
        g.FillRectangle(&reportBg,
            RectF(x, reportY, contentArea.Width - 20.0f, reportHeight));
        g.DrawRectangle(&borderPen,
            RectF(x, reportY, contentArea.Width - 20.0f, reportHeight));

        int startLine = 0;
        if (offsetY > 0) startLine = (int)(offsetY / lineHeight);
        int maxLines = (int)(reportHeight / lineHeight) + 1;
        int endLine = (std::min)(startLine + maxLines, (int)lines.size());
        for (int i = startLine; i < endLine; ++i) {
            float yPos = reportY + 4.0f + (i - startLine) * lineHeight - offsetY;
            g.DrawString(lines[i].c_str(), -1, &font,
                PointF(x + 6.0f, yPos), &textBrush);
        }
    }
}

// Функция, выполняющая разблокировку и формирующая лог
static std::wstring PerformUnlock(bool immediate, bool fixBcd, bool fixAcl, bool fixBoot,
    bool fixIfeo, bool repairSystemFiles) {
    std::wstring log;
    int unlockedCount = 0, failedCount = 0;

    if (UnlockTools::IsRecoveryEnvironment()) {
        const std::wstring selectedDrive = UnlockTools::GetOfflineDriveHint();
        const auto installations = UnlockTools::ScanDrivesWithWindows();
        const bool selectedTargetFound = !selectedDrive.empty() &&
            std::any_of(installations.begin(), installations.end(),
                [&](const std::wstring& drive) {
                    return _wcsicmp(drive.c_str(), selectedDrive.c_str()) == 0;
                });
        if (!selectedTargetFound) {
            return L"WINPE-RE: выбранный Windows-диск не найден. Проверьте выбор диска в настройках; X: не изменялся.";
        }

        if (!immediate)
            return L"WINPE-RE · целевая Windows: " + selectedDrive + L"Windows\r\n\r\n" +
                UnlockTools::GetBestUnlockReport(false);

        SYSTEMTIME backupTime{};
        GetLocalTime(&backupTime);
        wchar_t backupName[64]{};
        swprintf_s(backupName, L"Backup_%04u%02u%02u_%02u%02u%02u",
            backupTime.wYear, backupTime.wMonth, backupTime.wDay,
            backupTime.wHour, backupTime.wMinute, backupTime.wSecond);
        const std::wstring backupPath = selectedDrive + L"SYSIM_RecoveryBackup\\" + backupName +
            L"_" + std::to_wstring(GetTickCount64());
        bool userBackupsComplete = true;
        bool systemHiveBackupComplete = true;
        std::wstring backupFailure;
        if (!CreateOfflineBackupDirectory(selectedDrive, backupPath, false, false,
            &userBackupsComplete, &backupFailure, &systemHiveBackupComplete)) {
            return L"WINPE-RE: обязательная копия SYSTEM/SOFTWARE/hosts не создана. "
                L"Изменения отменены.\r\n" + backupFailure;
        }

        log = L"WINPE-RE · целевая Windows: " + selectedDrive + L"Windows\r\n" +
            L"Резервная копия: " + backupPath + L"\r\n" + backupFailure + L"\r\n" +
            UnlockTools::GetBestUnlockReport(true, userBackupsComplete,
                systemHiveBackupComplete);
        if (!repairSystemFiles) return log;

        log += L"\r\n--- BSOD 0xC000021A: офлайн-проверка файлов Windows ---\r\n";
        const bool systemFilesOk = UnlockTools::RepairOfflineSystemFiles(
            backupPath + L"\\Scratch", log);
        log += systemFilesOk
            ? L"Офлайн DISM и SFC завершились успешно. Перезагрузите компьютер и проверьте результат.\r\n"
            : L"Офлайн DISM или SFC завершились с ошибкой. Текст и коды команд приведены выше.\r\n";
        if (fixBcd || fixAcl || fixBoot || fixIfeo)
            log += L"\r\nВ WinRE BCD/ACL/Boot/IFEO online-действия пропущены; изменялась только выбранная Windows.";
        return log;
    }

    auto restrictions = UnlockTools::GetKnownRestrictions();
    for (const auto& r : restrictions) {
        if (!UnlockTools::IsRestricted(r))
            continue;

        if (immediate) {
            if (UnlockTools::UnlockRestriction(r)) {
                ++unlockedCount;
                std::wstring hiveStr;
                for (HKEY h : r.hives) {
                    if (h == HKEY_CURRENT_USER) hiveStr = L"HKCU";
                    else if (h == HKEY_LOCAL_MACHINE) hiveStr = L"HKLM";
                    else hiveStr = L"UNKNOWN";
                }
                log += L"Разблокировано: " + r.description +
                    L" (" + hiveStr + L"\\" + r.subKey + L"\\" + r.valueName +
                    L" = " + std::to_wstring(r.disableValue) + L")\r\n";
            }
            else {
                ++failedCount;
                log += L"Ошибка: " + r.description + L"\r\n";
            }
        }
        else {
            log += L"Обнаружена блокировка: " + r.description + L"\r\n";
        }
    }

    if (immediate) {
        bool hasDisallowRun =
            UnlockTools::HasDisallowRunAt(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer") ||
            UnlockTools::HasDisallowRunAt(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
        if (hasDisallowRun) {
            if (UnlockTools::ClearDisallowRun()) {
                log += L"Разблокировано: DisallowRun (удалены все записи)\r\n";
                ++unlockedCount;
            }
            else {
                log += L"Ошибка: не удалось очистить DisallowRun\r\n";
                ++failedCount;
            }
        }

        bool hasIFEO = UnlockTools::HasIFEODebuggerAt(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options");
        if (hasIFEO) {
            if (UnlockTools::ClearImageFileExecutionOptions()) {
                log += L"Разблокировано: IFEO FULL (online)\r\n";
                ++unlockedCount;
            }
            else {
                log += L"Ошибка: не удалось очистить IFEO FULL (online)\r\n";
                ++failedCount;
            }
        }
        /*
        const auto offlineDrives = UnlockTools::ScanDrivesWithWindows();
        if (!offlineDrives.empty()) {
            if (UnlockTools::ClearOfflineIFEO(log)) {
                log += L"Разблокировано: IFEO FULL (offline Windows)\r\n";
                ++unlockedCount;
            }
            else {
                log += L"Ошибка: не удалось очистить IFEO FULL (offline Windows)\r\n";
                ++failedCount;
            }
        }*/
    }

    if (fixBcd && immediate) {
        if (UnlockTools::IsBcdSafeBootEnabled()) {
            if (UnlockTools::ClearBcdSafeBoot()) {
                log += L"BCD: удалено значение safeboot\r\n";
                ++unlockedCount;
            }
            else {
                log += L"BCD: не удалось удалить safeboot\r\n";
                ++failedCount;
            }
        }
        else {
            log += L"BCD: safeboot не обнаружен\r\n";
        }
    }

    if (fixAcl && immediate) {
        wchar_t win[MAX_PATH] = {};
        if (GetWindowsDirectoryW(win, MAX_PATH)) {
            std::wstring aclLog;
            if (UnlockTools::ResetAclOnPath(win, true, aclLog)) {
                log += L"ACL: сброшены права на папку Windows\r\n";
                if (!aclLog.empty()) log += aclLog + L"\r\n";
                ++unlockedCount;
            }
            else {
                log += L"ACL: ошибка при сбросе прав\r\n";
                ++failedCount;
            }
        }
        else {
            log += L"ACL: не удалось определить папку Windows\r\n";
        }
    }

    if (fixBoot && immediate) {
        std::wstring bootLog;
        if (UnlockTools::RepairBootRecords(bootLog)) {
            log += L"Boot: выполнен ремонт записей загрузки\r\n";
            if (!bootLog.empty()) log += bootLog + L"\r\n";
            ++unlockedCount;
        }
        else {
            log += L"Boot: ошибка при ремонте загрузки\r\n";
            ++failedCount;
        }
    }

    if (immediate) {
        UnlockTools::NotifySystemChanges(true, true, true, true);

        UnlockTools::RestartExplorer();
        log += L"Explorer перезапущен для применения изменений UI.\r\n";

        std::wstring header;
        header += L"Разблокировано: " + std::to_wstring(unlockedCount) + L"\r\n";
        header += L"Ошибок: " + std::to_wstring(failedCount) + L"\r\n\r\n";
        log = header + log;
    }
    else {
        log = L"--- Список обнаруженных блокировок ---\r\n" + log;
    }

    return log;
}

static std::wstring PerformOfflineLogonFileRepair() {
    const std::wstring selectedDrive = UnlockTools::GetOfflineDriveHint();
    const auto installations = UnlockTools::ScanDrivesWithWindows();
    const bool selectedTargetFound = !selectedDrive.empty() &&
        std::any_of(installations.begin(), installations.end(),
            [&](const std::wstring& drive) {
                return _wcsicmp(drive.c_str(), selectedDrive.c_str()) == 0;
            });
    if (!selectedTargetFound)
        return L"WINPE-RE: выбранная Windows не найдена. X: не изменялся.";

    SYSTEMTIME backupTime{};
    GetLocalTime(&backupTime);
    wchar_t backupName[64]{};
    swprintf_s(backupName, L"Backup_%04u%02u%02u_%02u%02u%02u",
        backupTime.wYear, backupTime.wMonth, backupTime.wDay,
        backupTime.wHour, backupTime.wMinute, backupTime.wSecond);
    const std::wstring backupPath = selectedDrive + L"SYSIM_RecoveryBackup\\" + backupName +
        L"_" + std::to_wstring(GetTickCount64());
    std::wstring backupFailure;
    if (!CreateOfflineBackupDirectory(selectedDrive, backupPath, false, true,
        nullptr, &backupFailure)) {
        return L"Резервирование SYSTEM/SOFTWARE/hosts и оригинальных utilman/sethc не выполнено. "
            L"SFC не запускался.\r\n" + backupFailure;
    }

    std::wstring report = L"WINPE-RE · целевая Windows: " + selectedDrive + L"Windows\r\n" +
        L"Резервные копии, включая оригинальные файлы входа: " + backupPath + L"\r\n\r\n";
    const bool repaired = UnlockTools::RepairOfflineLogonFiles(report);
    report += repaired
        ? L"SFC завершил проверку выбранных системных файлов. Перезагрузите компьютер и проверьте вход.\r\n"
        : L"SFC не подтвердил успешное восстановление. Смотрите вывод и код выше; копии сохранены.\r\n";
    return report;
}

struct UnlockTask {
    bool immediate = false;
    bool fixBcd = false;
    bool fixAcl = false;
    bool fixBoot = false;
    bool fixIfeo = false;
    HWND hwnd = nullptr;
    bool scanOnly = false;
    bool repairLogonFiles = false;
    bool showCompletionDialog = false;
    bool repairSystemFiles = true;
};

static unsigned __stdcall UnlockWorkerProc(void* param) {
    auto* task = static_cast<UnlockTask*>(param);
    if (!task) return 0;

    std::wstring log;
    if (task->scanOnly)
        log = UnlockTools::GetBestUnlockReport(false);
    else if (task->repairLogonFiles)
        log = PerformOfflineLogonFileRepair();
    else
        log = PerformUnlock(task->immediate, task->fixBcd, task->fixAcl,
            task->fixBoot, task->fixIfeo, task->repairSystemFiles);

    if (task->hwnd && IsWindow(task->hwnd)) {
        auto* report = new std::wstring(log);
        const WPARAM completionCode = task->showCompletionDialog ? 2 :
            (task->scanOnly ? 1 : 0);
        if (!PostMessageW(task->hwnd, WM_UNLOCK_COMPLETE, completionCode,
            reinterpret_cast<LPARAM>(report))) delete report;
    }

    delete task;
    return 0;
}

static void StartWinPeOperation(bool repairLogonFiles, bool repairSystemFiles) {
    HWND hwnd = App::Instance() ? App::Instance()->GetHWND() : nullptr;
    if (!hwnd || !UnlockTools::IsRecoveryEnvironment() || g_unlockInProgress) return;

    const std::wstring selectedDrive = UnlockTools::GetOfflineDriveHint();
    const auto installations = UnlockTools::ScanDrivesWithWindows();
    const bool selectedTargetFound = !selectedDrive.empty() &&
        std::any_of(installations.begin(), installations.end(),
            [&](const std::wstring& drive) {
                return _wcsicmp(drive.c_str(), selectedDrive.c_str()) == 0;
            });
    if (!selectedTargetFound) {
        MessageBoxW(hwnd,
            L"Целевая Windows не найдена. Выберите доступный Windows-диск в настройках.",
            L"WINPE-RE", MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring prompt = repairLogonFiles
        ? std::wstring(L"Сохранить копии и проверить/восстановить оригинальные utilman.exe и sethc.exe в ") +
            selectedDrive + L"Windows?"
        : repairSystemFiles
            ? std::wstring(L"Сохранить копии, снять найденные политики и выполнить DISM/SFC для ") +
                selectedDrive + L"Windows?"
            : std::wstring(L"Сохранить копии и снять найденные поддерживаемые политики в ") +
                selectedDrive + L"Windows?";
    if (MessageBoxW(hwnd, (prompt +
        L"\r\n\r\nБудет изменена только выбранная Windows. X: не затрагивается. Продолжить?").c_str(),
        L"WINPE-RE", MB_YESNO | MB_ICONWARNING) != IDYES) return;

    auto* task = new UnlockTask{
    !repairLogonFiles, false, false, false, false, hwnd, false,
    repairLogonFiles, true, repairSystemFiles
    };

    g_unlockInProgress = true;
    g_lastReport = repairLogonFiles
        ? L"WINPE-RE: сохранение копий и проверка utilman/sethc...\r\n"
        : L"WINPE-RE: сохранение копий и восстановление выбранной Windows...\r\n";
    InvalidateRect(hwnd, nullptr, TRUE);

    unsigned threadId = 0;
    HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, 0, UnlockWorkerProc, task, 0, &threadId));
    if (!thread) {
        delete task;
        g_unlockInProgress = false;
        g_lastReport = L"Не удалось запустить WINPE-RE.";
        InvalidateRect(hwnd, nullptr, TRUE);
        MessageBoxW(hwnd, g_lastReport.c_str(), L"WINPE-RE", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(thread);
}

void RunWinPeFullRepair() {
    StartWinPeOperation(false, true);
}

void RunWinPeUnlockPolicies() {
    StartWinPeOperation(false, false);
}

void RunWinPeLogonFilesRepair() {
    StartWinPeOperation(true, false);
}

static void StartUnlockScan() {
    if (g_unlockInProgress) return;
    HWND hwnd = App::Instance() ? App::Instance()->GetHWND() : nullptr;
    if (!hwnd) return;

    auto* task = new UnlockTask{};
    task->hwnd = hwnd;
    task->scanOnly = true;
    g_unlockInProgress = true;
    g_lastReport = L"Сканирование блокировок...\r\n";
    InvalidateRect(hwnd, nullptr, TRUE);

    unsigned threadId = 0;
    HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, 0, UnlockWorkerProc, task, 0, &threadId));
    if (!thread) {
        delete task;
        g_unlockInProgress = false;
        g_lastReport = L"Не удалось запустить фоновое сканирование.\r\n";
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    CloseHandle(thread);
}

void RunWinPeScan() {
    HWND hwnd = App::Instance() ? App::Instance()->GetHWND() : nullptr;
    if (!hwnd || !UnlockTools::IsRecoveryEnvironment() || g_unlockInProgress) return;
    const std::wstring selectedDrive = UnlockTools::GetOfflineDriveHint();
    const auto installations = UnlockTools::ScanDrivesWithWindows();
    const bool selectedTargetFound = !selectedDrive.empty() &&
        std::any_of(installations.begin(), installations.end(),
            [&](const std::wstring& drive) {
                return _wcsicmp(drive.c_str(), selectedDrive.c_str()) == 0;
            });
    if (!selectedTargetFound) {
        MessageBoxW(hwnd, L"Выберите доступную установленную Windows в настройках.",
            L"WINPE-RE", MB_OK | MB_ICONWARNING);
        return;
    }

    auto* task = new UnlockTask{};
    task->hwnd = hwnd;
    task->scanOnly = true;
    task->showCompletionDialog = true;
    g_unlockInProgress = true;
    g_lastReport = L"WINPE-RE: сканирование " + selectedDrive + L"Windows...\r\n";
    InvalidateRect(hwnd, nullptr, TRUE);
    unsigned threadId = 0;
    HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, 0, UnlockWorkerProc, task, 0, &threadId));
    if (!thread) {
        delete task;
        g_unlockInProgress = false;
        g_lastReport = L"Не удалось запустить сканирование WINPE-RE.";
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    CloseHandle(thread);
}

// Обработка кликов
bool OnUnlockClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);

    if (UnlockTools::IsRecoveryEnvironment()) {
        HWND owner = App::Instance()->GetHWND();
        if (HitTestRect(g_ifeoScanRect, fx, fy)) {
            g_lastReport = UnlockTools::GetIfeoDebuggerReport(false);
            InvalidateRect(owner, nullptr, TRUE);
            return true;
        }
        if (HitTestRect(g_ifeoDeleteRect, fx, fy)) {
            if (MessageBoxW(owner,
                L"Удалить найденные IFEO Debugger из выбранной Windows?",
                L"Удаление IFEO", MB_YESNO | MB_ICONWARNING) == IDYES) {
                g_lastReport = UnlockTools::GetIfeoDebuggerReport(true);
                InvalidateRect(owner, nullptr, TRUE);
            }
            return true;
        }
        if (HitTestRect(g_refreshButtonRect, fx, fy)) {
            StartUnlockScan();
            return true;
        }
        return false;
    }

    if (HitTestRect(g_checkboxRect, fx, fy)) {
        g_immediateUnlock = !g_immediateUnlock;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (HitTestRect(g_bcdCheckboxRect, fx, fy)) {
        g_fixBcdSafeBoot = !g_fixBcdSafeBoot;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (HitTestRect(g_aclCheckboxRect, fx, fy)) {
        g_fixAcl = !g_fixAcl;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (HitTestRect(g_bootCheckboxRect, fx, fy)) {
        g_fixBoot = !g_fixBoot;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (HitTestRect(g_ifeoCheckboxRect, fx, fy)) {
        g_fixIfeo = !g_fixIfeo;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    if (HitTestRect(g_refreshButtonRect, fx, fy)) {
        StartUnlockScan();
        return true;
    }

    if (HitTestRect(g_buttonRect, fx, fy)) {
        if (g_immediateUnlock) {
            if (g_unlockInProgress) {
                return true;
            }

            g_unlockInProgress = true;
            g_lastReport = L"Запуск разблокировки...\r\n";
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);

            auto* task = new UnlockTask{
                true,
                g_fixBcdSafeBoot,
                g_fixAcl,
                g_fixBoot,
                g_fixIfeo,
                App::Instance()->GetHWND()
            };

            unsigned threadId = 0;
            HANDLE thread = (HANDLE)_beginthreadex(
                nullptr, 0, UnlockWorkerProc, task, 0, &threadId);
            if (!thread) {
                g_unlockInProgress = false;
                g_lastReport = L"Не удалось запустить фоновую разблокировку.\r\n";
                InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
                MessageBoxW(App::Instance()->GetHWND(),
                    L"Не удалось запустить фоновую разблокировку.",
                    L"Разблокировка", MB_OK | MB_ICONERROR);
                return true;
            }
            CloseHandle(thread);
            return true;
        }

        StartUnlockScan();
        return true;
    }

    return false;
}