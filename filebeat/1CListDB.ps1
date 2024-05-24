<#*
 * This file is part of the elastic-onec (https://github.com/toreonify/elastic-onec).
 * Copyright (c) 2024 Ivan Korytov <toreonify@outlook.com>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *#>

param (
    [switch] $ListDatabases,
    [string[]] $Databases,
    [string] $ScriptPath
)

Import-Module "Microsoft.PowerShell.Management"

$ErrorActionPreference = "Stop"

Set-Variable -Name LogsPath -Value "C:\Program Files\1cv8\srvinfo\reg_1541"
Set-Variable -Name DBListPath -Value "$LogsPath\1CV8Clst.lst"

$content = Get-Content -Raw "$DBListPath"
$List = Select-String '{([a-z0-9]{8}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{12}),"(.*?)",".*".".*"' -Input $content -AllMatches

if ($ListDatabases) {
    $List.Matches | ForEach {Write-Output ('"' + $_.Groups[1].Value + '": "' + $_.Groups[2].Value + '"')}
} else {
	if ($Databases.Count -gt 0) {
		$Matches = @()
		foreach ($Database in $Databases) {
			$Found = ($List.Matches | Where-Object {$_.Groups[2].Value -eq $Database})

			if ($Found) {
				$Matches += $Found.Groups[1].Value
			} else {
				Write-Output "Database $Database not found"
			}
		}
		
		$DBPaths = ""
		
		Write-Output "Filebeat logs path:"
		foreach ($Match in $Matches) {
			Write-Output "  - $LogsPath\$Match\1Cv8Log\*.lgp"
			
			$DBPaths += " `\```"$LogsPath\$Match\1Cv8Log\1Cv8.lgf`\```""
		}
		
		if ($ScriptPath) {
			Write-Output "1CSendDictionaries startup parameters (for PowerShell use):"
			Write-Output "  `"`\```"$ScriptPath`\```" `"$DBPaths`""
		}
	} else {
		Write-Output "Please enter at least one database name."
	}
}
