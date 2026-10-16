$content = Get-Content -Raw -Path 'C:\Users\NIRBHAY\.gemini\antigravity-ide\brain\0627a47a-440f-41dc-8541-7fbf68b81dfc\.system_generated\logs\transcript_full.jsonl'
$lines = $content -split "`r?`n"
foreach ($line in $lines) {
    if ($line.Contains("MASTER IMPLEMENTATION PROMPT")) {
        $obj = ConvertFrom-Json $line
        [System.IO.File]::WriteAllText("d:\Snowflake\DB\master_prompt_extracted.txt", $obj.content)
        Write-Host "SUCCESSFULLY EXTRACTED MASTER PROMPT!"
        break
    }
}
