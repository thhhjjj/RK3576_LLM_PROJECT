# push_or_print.ps1 — 提交本地改动并推送；失败则打印可手动执行的命令
# 用法:
#   .\push_or_print.ps1
#   .\push_or_print.ps1 "chore: sync local changes"

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$Branch = git rev-parse --abbrev-ref HEAD
if ($LASTEXITCODE -ne 0) { throw "不在 git 仓库中" }

$Msg = if ($args.Count -ge 1 -and $args[0]) { $args[0] } else { "chore: sync local changes" }

git add -A
$staged = git diff --cached --name-only
if (-not $staged) {
    Write-Host "无新改动可提交"
} else {
    git commit -m $Msg
    if ($LASTEXITCODE -ne 0) { throw "commit 失败" }
}

git push -u origin $Branch
if ($LASTEXITCODE -eq 0) {
    Write-Host "推送成功: origin/$Branch"
    exit 0
}

$Root = (Get-Location).Path
Write-Host ""
Write-Host "===== 推送失败，请手动执行 ====="
Write-Host @"
cd `"$Root`"
git status
git pull --rebase origin $Branch
# 若需认证（HTTPS）:
#   gh auth login
# 或改用 SSH:
#   git remote set-url origin git@github.com:thhhjjj/RK3576_LLM_PROJECT.git
git push -u origin $Branch
"@
exit 1
