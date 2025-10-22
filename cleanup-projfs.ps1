#!/usr/bin/env pwsh
# ProjFS Cleanup Script
# Comprehensive cleanup for ProjFS mount points and test artifacts

param(
    [string]$MountPoint = "",
    [switch]$KillNode = $true,
    [switch]$Force = $false
)

$ErrorActionPreference = "Continue"

Write-Host ""
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "          ProjFS Mount Point Cleanup Script" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Kill Node.js processes
if ($KillNode) {
    Write-Host "[1/5] Killing Node.js processes..." -ForegroundColor Yellow

    $nodeProcesses = Get-Process -Name node -ErrorAction SilentlyContinue
    if ($nodeProcesses) {
        $count = $nodeProcesses.Count
        Write-Host "  Found $count Node.js process(es)" -ForegroundColor Gray

        foreach ($proc in $nodeProcesses) {
            try {
                $proc | Stop-Process -Force -ErrorAction Stop
                Write-Host "  + Killed process PID $($proc.Id)" -ForegroundColor Green
            } catch {
                Write-Host "  - Failed to kill PID $($proc.Id): $($_.Exception.Message)" -ForegroundColor Red
            }
        }

        Start-Sleep -Milliseconds 500
        Write-Host "  + All Node.js processes terminated" -ForegroundColor Green
    } else {
        Write-Host "  + No Node.js processes running" -ForegroundColor Green
    }
} else {
    Write-Host "[1/5] Skipping Node.js process cleanup (disabled)" -ForegroundColor Gray
}

Write-Host ""

# Step 2: Find mount points
Write-Host "[2/5] Identifying mount points to clean..." -ForegroundColor Yellow

$mountPoints = @()

if ($MountPoint) {
    if (Test-Path $MountPoint) {
        $mountPoints += $MountPoint
        Write-Host "  + User-specified mount point: $MountPoint" -ForegroundColor Green
    } else {
        Write-Host "  ! Mount point not found: $MountPoint" -ForegroundColor Yellow
    }
} else {
    $tempDir = [System.IO.Path]::GetTempPath()
    $patterns = @(
        "OneFiler-Test*",
        "PhantomTest-*",
        "refinio-api-*-instance"
    )

    foreach ($pattern in $patterns) {
        $found = Get-ChildItem -Path $tempDir -Directory -Filter $pattern -ErrorAction SilentlyContinue
        foreach ($dir in $found) {
            $mountPoints += $dir.FullName
        }
    }

    if ($mountPoints.Count -eq 0) {
        Write-Host "  + No test mount points found" -ForegroundColor Green
    } else {
        Write-Host "  + Found $($mountPoints.Count) mount point(s):" -ForegroundColor Green
        foreach ($mp in $mountPoints) {
            Write-Host "    - $mp" -ForegroundColor Gray
        }
    }
}

Write-Host ""

# Step 3: Clear ProjFS state
Write-Host "[3/5] Clearing ProjFS virtualization state..." -ForegroundColor Yellow

$clearedCount = 0
foreach ($mp in $mountPoints) {
    $projfsDir = Join-Path $mp ".projfs"

    if (Test-Path $projfsDir) {
        try {
            $projfsItem = Get-Item $projfsDir -Force
            if ($projfsItem.Attributes -band [System.IO.FileAttributes]::Hidden) {
                $projfsItem.Attributes = $projfsItem.Attributes -bxor [System.IO.FileAttributes]::Hidden
            }
            if ($projfsItem.Attributes -band [System.IO.FileAttributes]::System) {
                $projfsItem.Attributes = $projfsItem.Attributes -bxor [System.IO.FileAttributes]::System
            }

            Remove-Item -Path $projfsDir -Recurse -Force -ErrorAction Stop
            Write-Host "  + Cleared $projfsDir" -ForegroundColor Green
            $clearedCount++
        } catch {
            Write-Host "  - Failed to clear $projfsDir : $($_.Exception.Message)" -ForegroundColor Red
        }
    }
}

if ($clearedCount -eq 0 -and $mountPoints.Count -gt 0) {
    Write-Host "  + No .projfs directories found" -ForegroundColor Green
}

Write-Host ""

# Step 4: Remove phantom placeholders
Write-Host "[4/5] Removing phantom placeholders..." -ForegroundColor Yellow

$phantomCount = 0
foreach ($mp in $mountPoints) {
    if (-not (Test-Path $mp)) {
        continue
    }

    try {
        $items = Get-ChildItem -Path $mp -Force -ErrorAction Stop | Where-Object {
            $_.Name -ne ".projfs" -and
            ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
        }

        foreach ($item in $items) {
            try {
                Remove-Item -Path $item.FullName -Recurse -Force -ErrorAction Stop
                Write-Host "  + Removed phantom: $($item.Name)" -ForegroundColor Green
                $phantomCount++
            } catch {
                Write-Host "  - Failed to remove $($item.Name): $($_.Exception.Message)" -ForegroundColor Red
            }
        }
    } catch {
        Write-Host "  ! Cannot scan $mp : $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

if ($phantomCount -eq 0) {
    Write-Host "  + No phantom placeholders found" -ForegroundColor Green
}

Write-Host ""

# Step 5: Remove mount points
Write-Host "[5/5] Removing mount point directories..." -ForegroundColor Yellow

$removedCount = 0
foreach ($mp in $mountPoints) {
    if (-not (Test-Path $mp)) {
        Write-Host "  + Already removed: $mp" -ForegroundColor Gray
        continue
    }

    try {
        if ($Force) {
            Get-ChildItem -Path $mp -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
                if ($_.Attributes -band [System.IO.FileAttributes]::ReadOnly) {
                    $_.Attributes = $_.Attributes -bxor [System.IO.FileAttributes]::ReadOnly
                }
            }
        }

        Remove-Item -Path $mp -Recurse -Force -ErrorAction Stop
        Write-Host "  + Removed: $mp" -ForegroundColor Green
        $removedCount++
    } catch {
        if ($_.Exception.Message -like "*being used by another process*") {
            Write-Host "  - Mount point in use: $mp" -ForegroundColor Red
            Write-Host "    Try running with -KillNode or -Force" -ForegroundColor Yellow
        } else {
            Write-Host "  - Failed to remove $mp : $($_.Exception.Message)" -ForegroundColor Red
        }
    }
}

Write-Host ""
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "                      Cleanup Complete" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host ""

Write-Host "Summary:" -ForegroundColor White
Write-Host "  Mount points processed: $($mountPoints.Count)" -ForegroundColor Gray
Write-Host "  ProjFS states cleared: $clearedCount" -ForegroundColor Gray
Write-Host "  Phantom placeholders removed: $phantomCount" -ForegroundColor Gray
Write-Host "  Directories removed: $removedCount" -ForegroundColor Gray
Write-Host ""

if ($mountPoints.Count -gt $removedCount) {
    Write-Host "WARNING: Some mount points could not be removed." -ForegroundColor Yellow
    Write-Host "   This usually means they are still in use by running processes." -ForegroundColor Yellow
    Write-Host "   Try: .\cleanup-projfs.ps1 -KillNode -Force" -ForegroundColor Yellow
    Write-Host ""
    exit 1
} else {
    Write-Host "SUCCESS: Cleanup complete!" -ForegroundColor Green
    Write-Host ""
    exit 0
}
