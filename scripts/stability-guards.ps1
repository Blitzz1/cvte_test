function Test-StabilityResource {
    param($Row,$Previous,[int]$HighCpuSamples=0,[int]$AppLimitMiB=512,[int]$ServerLimitMiB=256,[double]$CpuLimitPct=20)
    $cpu=$null
    if($null -ne $Previous -and $Row.elapsed_s -gt $Previous.elapsed_s){
        $cpu=100*(($Row.cpu_seconds+$Row.server_cpu_seconds)-($Previous.cpu_seconds+$Previous.server_cpu_seconds))/($Row.elapsed_s-$Previous.elapsed_s)/[Math]::Max(1,$Row.logical_processors)
    }
    $high=if($null -ne $cpu -and $cpu -gt $CpuLimitPct){$HighCpuSamples+1}else{0}
    $reason=if($Row.private_bytes -gt $AppLimitMiB*1MB){'app_private_memory_limit'}elseif($Row.server_private_bytes -gt $ServerLimitMiB*1MB){'server_private_memory_limit'}elseif($high -ge 2){'sustained_combined_cpu_limit'}else{''}
    return @{stop=($reason -ne '');reason=$reason;combined_cpu_pct=$cpu;high_cpu_samples=$high}
}
