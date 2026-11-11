$voice = New-Object -ComObject SAPI.SpVoice
$voice.Voice = $voice.GetVoices('Language=804').Item(0)
$voice.Volume = 75
$startText = -join ([char[]](0x5f00,0x59cb,0x91c7,0x96c6))
$stopText = -join ([char[]](0x7ed3,0x675f,0x91c7,0x96c6))
foreach ($item in @(@('start', $startText), @('stop', $stopText))) {
    $stream = New-Object -ComObject SAPI.SpFileStream
    $stream.Format.Type = 22 # 22.05 kHz, 16 bit, mono
    $stream.Open((Join-Path (Get-Location) ('build/recording_' + $item[0] + '.wav')), 3)
    $voice.AudioOutputStream = $stream
    [void]$voice.Speak($item[1])
    $stream.Close()
}
