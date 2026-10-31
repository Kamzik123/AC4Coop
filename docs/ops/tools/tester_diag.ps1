$game = "C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed IV Black Flag"
$launcher = "C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher"

"=== is the game running? ==="
Get-Process AC4BFSP -ErrorAction SilentlyContinue |
    Select-Object Id, StartTime, MainWindowTitle | Format-Table -AutoSize

"=== what is in the game folder (ini + logs) ==="
Get-ChildItem -LiteralPath $game -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^phase\d*_log_|^AC4Coop' } |
    Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize

"=== any phase/all log files anywhere under the launcher tree ==="
Get-ChildItem -LiteralPath $launcher -Recurse -ErrorAction SilentlyContinue -Include 'phase0_log_*.txt','phase1_log_*.txt' |
    Select-Object FullName, Length, LastWriteTime | Format-Table -AutoSize

"=== is the ASI loader present next to the exe? ==="
Get-ChildItem -LiteralPath $game -Filter 'dinput8.dll' -ErrorAction SilentlyContinue |
    Select-Object Name, Length | Format-Table -AutoSize
Get-ChildItem -LiteralPath $game -Filter 'AC4Coop.asi' -ErrorAction SilentlyContinue |
    Select-Object Name, Length | Format-Table -AutoSize
