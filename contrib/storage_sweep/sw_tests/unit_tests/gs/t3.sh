#!/bin/bash
begin_test=2500
end_test=2560
total_test_time=$((end_test-begin_test))
echo "total_test_time: $total_test_time"
show_test_duration()
{
    local total_test_time
    total_test_time=$1
    local t_hsec
    local t_hor
    local t_min
    local t_sec
    
    t_hor=$((total_test_time / 3600))
    if [[ "$t_hor" -gt 0 ]]; then
	t_hsec=$((t_hor * 3600))
	t_hsec=$((total_test_time - t_hsec))
	t_min=$((t_hsec / 60))
        t_sec=$((total_test_time % 60))
	echo "Total test time: ${t_hor}h:${t_min}m:${t_sec}s"
    else
	t_min=$((total_test_time / 60))
        t_sec=$((total_test_time % 60))	
	echo "Total test time: ${t_min}m:${t_sec}s"
    fi
}

# main()
{
    show_test_duration "$total_test_time"
    exit 0
}
