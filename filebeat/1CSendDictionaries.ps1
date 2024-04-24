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
    [string] $FileName
)

Import-Module "Microsoft.PowerShell.Management"

$ErrorActionPreference = "Stop"

Set-Variable -Name LogstashTCPAddress -Value 127.0.0.1
Set-Variable -Name LogstashTCPPort -Value 12000

Set-Variable -Name LockFilePath -Value "$env:TEMP\1CLogSend.lock"

function Send-JsonOverTcp {
    param (
        [ValidateNotNullOrEmpty()]
        [string] $Server,
        [int] $Port,
        $Object
    )

    $Ip = [System.Net.Dns]::GetHostAddresses($Server)
    $Address = [System.Net.IPAddress]::Parse($Ip)

    $Socket = New-Object System.Net.Sockets.TCPClient($Address,$Port)
    $Stream = $Socket.GetStream()
 
    $Writer = New-Object System.IO.StreamWriter($Stream)
    $Writer.WriteLine(($Object | ConvertTo-Json -Compress))

    $Writer.Flush()
    $Stream.Close()
    $Socket.Close()
}

function Send-Dictionaries {
    param (
        $DictionaryPath
    )

    $Event = @{
        'service' = @{
            'type' = 'onec'
        }
        'event' = @{
            'module' = 'onec'
            'dataset' = 'onec.map'
        }
        'input' = @{
            'type' = 'log'
        }
        'fileset' = @{
            'name' = 'map'
        }
        'log' = @{
            'flags' = 'multiline'
            'offset' = 0
            'file' = @{
                'path' = "$DictionaryPath"
            }
        }
        '@version' = '1'
        'message' = ''
        'ecs' = @{
            'version' = '8.0.0'
        }
    }

    $content = Get-Content -Raw "$DictionaryPath"
    $Event.message = $content.ToString().Replace("`r","");

    Send-JsonOverTcp -Server $LogstashTCPAddress -Port $LogstashTCPPort -Object $Event

    Log -Type Information -ID 1000 -Message "Dictionaries sent."
}

function Send-DBList {
	param (
        $ListPath
    )
	
    $Event = @{
        'service' = @{
            'type' = 'onec'
        }
        'event' = @{
            'module' = 'onec'
            'dataset' = 'onec.dblist'
        }
        'input' = @{
            'type' = 'log'
        }
        'fileset' = @{
            'name' = 'dblist'
        }
        'log' = @{
            'flags' = 'multiline'
            'offset' = 0
            'file' = @{
                'path' = "$DBListPath"
            }
        }
        '@version' = '1'
        'message' = ''
        'ecs' = @{
            'version' = '8.0.0'
        }
    }

    $content = Get-Content -Raw "$ListPath"
    $List = Select-String '{([a-z0-9]{8}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{12}),"(.*?)",".*".".*"' -Input $content -AllMatches

    $content = $List.Matches | ForEach {Write-Output ('"' + $_.Groups[1].Value + '": "' + $_.Groups[2].Value + '"')}
    $Event.message = ($content | Out-String).Replace("`r","");
    
    Send-JsonOverTcp -Server $LogstashTCPAddress -Port $LogstashTCPPort -Object $Event

    Log -Type Information -ID 1000 -Message "Database list sent."
}

function Log {
    param (
        $Type,
        $ID,
        $Message
    )
	
    Write-EventLog –LogName Application –Source "1CSendDictionaries" –EntryType $Type –EventID $ID –Message "$Message"
}

try {
    Log -Type Information -ID 1000 -Message "1C dictionary sender started"
    
	if ([System.IO.Path]::GetExtension($FileName) -eq ".lst") {
		Send-DBList -ListPath "$FileName"
	} else {
		if ([System.IO.Path]::GetExtension($FileName) -eq ".lgf") {
			Send-Dictionaries -DictionaryPath "$FileName"
		} else {
			Log -Type Error -ID 2000 -Message "Unknown file type '$Filename'."
		}
	}
}
catch {
    Write-Output "Exception: $_"
    Log -Type Error -ID 2000 -Message "Exception: $_"
}
finally {

}
