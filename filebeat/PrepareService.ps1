param(
	[string]
	$ExecutablePath,
	[string]
	$StartupParameters,
	[switch]
	$Update
)

$ErrorActionPreference = "Stop"

if ($StartupParameters -and $ExecutablePath) {
	if ([System.Diagnostics.EventLog]::SourceExists("1CSendDictionaries") -eq $False) {
		New-EventLog -LogName Application -Source "1CSendDictionaries"
	}

	if ($Update) {
		sc.exe config 1cdsend binPath= "\`"$ExecutablePath\`" $StartupParameters"
	} else {
		New-Service -Name 1cdsend -BinaryPathName "$ExecutablePath $StartupParameters" -StartupType "Manual" -Description "Служба для отправки словарей журналов регистрации 1С в Logstash." -DisplayName "Отправка словарей журнала регистрации 1С"
	}
} else {
	Write-Error "Executable path and startup parameters not provided."
}
