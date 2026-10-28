param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference='Stop'
if(-not(Test-Path (Join-Path $Directory 'result.json'))){throw 'Wait for collection completion'}
Add-Type -AssemblyName System.Windows.Forms.DataVisualization
$rows=@(Get-Content (Join-Path $Directory 'samples.jsonl') -Encoding UTF8|ForEach-Object{$_|ConvertFrom-Json})
function Flat($row){if($row.chains.PSObject.Properties.Name -contains 'value'){@($row.chains.value)}else{@($row.chains)}}
$chart=[System.Windows.Forms.DataVisualization.Charting.Chart]::new()
$chart.Width=1500;$chart.Height=1500;$chart.BackColor=[Drawing.Color]::White
$title=$chart.Titles.Add('Four-chain stability: sampled evidence')
$title.Font=[Drawing.Font]::new('Segoe UI',18)
$labels=@('Private memory (MiB)','Output (fps)','Measured latency (ms)','Combined CPU (%)')
for($i=0;$i -lt 4;$i++){
    $area=[System.Windows.Forms.DataVisualization.Charting.ChartArea]::new("panel$i")
    $area.Position=[System.Windows.Forms.DataVisualization.Charting.ElementPosition]::new(3,[single](5+$i*23),94,22)
    $area.AxisX.Title=if($i -eq 3){'Elapsed time (minutes)'}else{''}
    $area.AxisY.Title=$labels[$i];$area.AxisX.Minimum=0;$area.AxisX.Maximum=60
    $area.AxisX.MajorGrid.LineColor=[Drawing.Color]::Gainsboro;$area.AxisY.MajorGrid.LineColor=[Drawing.Color]::Gainsboro
    $chart.ChartAreas.Add($area)
}
$legend=$chart.Legends.Add('Legend');$legend.Docking='Bottom';$legend.Font=[Drawing.Font]::new('Segoe UI',10)
function Line([string]$Name,[int]$Panel,[string]$Color,[bool]$Legend=$true){
    $series=$chart.Series.Add($Name);$series.ChartType='Line';$series.ChartArea="panel$Panel";$series.Color=[Drawing.ColorTranslator]::FromHtml($Color);$series.BorderWidth=2;$series.IsVisibleInLegend=$Legend;return $series
}
$app=Line 'Monitor private' 0 '#2563eb'
$server=Line 'Media server private' 0 '#059669'
$cpu=Line 'Combined CPU' 3 '#dc2626' $false
$colors=@('#2563eb','#059669','#d97706','#7c3aed');$fps=@{};$latency=@{};$index=0
foreach($id in @((Flat $rows[0])|ForEach-Object id|Sort-Object)){
    $fps[$id]=Line $id 1 $colors[$index%4]
    $latency[$id]=Line ($id+' latency') 2 $colors[$index%4] $false
    $index++
}
foreach($row in $rows){
    $x=$row.elapsed_s/60
    $null=$app.Points.AddXY($x,$row.private_bytes/1MB)
    if($null -ne $row.server_private_bytes){$null=$server.Points.AddXY($x,$row.server_private_bytes/1MB)}
    if($null -ne $row.combined_cpu_pct){$null=$cpu.Points.AddXY($x,$row.combined_cpu_pct)}
    foreach($c in (Flat $row)){
        if($null -ne $c.output_fps){$null=$fps[$c.id].Points.AddXY($x,$c.output_fps)}
        if($null -ne $c.latency_ms){$null=$latency[$c.id].Points.AddXY($x,$c.latency_ms)}
    }
}
$path=Join-Path ([IO.Path]::GetFullPath($Directory)) 'stability.png'
try{$chart.SaveImage($path,[System.Windows.Forms.DataVisualization.Charting.ChartImageFormat]::Png)}finally{$chart.Dispose()}
Write-Output $path
