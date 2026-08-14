# Upload a YOLOv5 .tflite + labels.txt to DetectX on an Axis camera.
# DetectX restarts itself after the upload so the new model becomes active.
# Usage: .\deploy_model.ps1 -Tflite path\model.tflite -Labels path\labels.txt `
#          -Description "..." [-Camera 192.168.1.141] [-User root] [-Pass pass]
param(
    [Parameter(Mandatory=$true)][string]$Tflite,
    [Parameter(Mandatory=$true)][string]$Labels,
    [string]$Description = "",
    [string]$Camera = "192.168.1.141",
    [string]$User = "root",
    [string]$Pass = "pass"
)
$ErrorActionPreference = "Stop"

$b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($Tflite))
$labelsText = [IO.File]::ReadAllText($Labels)
$payload = @{ description = $Description; tflite_b64 = $b64; labels_content = $labelsText } | ConvertTo-Json -Compress

$tmp = Join-Path $env:TEMP "detectx_model_payload.json"
[IO.File]::WriteAllText($tmp, $payload)
try {
    Write-Host "Uploading $([IO.Path]::GetFileName($Tflite)) ($([math]::Round((Get-Item $Tflite).Length/1MB,1)) MB) to $Camera ..."
    curl.exe -s --digest -u "${User}:${Pass}" -X POST "http://$Camera/local/detectx/model" `
        -H "Content-Type: application/json" --data-binary "@$tmp"
    Write-Host "`nWaiting for DetectX to restart..."
    Start-Sleep -Seconds 15
    # the model-endpoint restart stops the app but does not reliably start it
    curl.exe -s --digest -u "${User}:${Pass}" "http://$Camera/axis-cgi/applications/control.cgi?action=start&package=detectx" | Out-Null
    for ($i = 0; $i -lt 10; $i++) {
        $info = (curl.exe -s --digest -u "${User}:${Pass}" "http://$Camera/local/detectx/model") -join ""
        if ($LASTEXITCODE -eq 0 -and $info -and $info -notmatch "error" -and $info -notmatch "Unavailable") {
            Write-Host "Model info reported by camera:"
            Write-Host $info
            return
        }
        Start-Sleep -Seconds 3
    }
    Write-Warning "DetectX did not come back within 30s - check http://$Camera/camera/index.html#/apps"
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
