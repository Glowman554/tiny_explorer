#!/bin/bash
# Compile with debug symbols but full optimization, no coverage
g++ -std=c++17 -O3 -g fetsim.cpp -o fetsim

# Run in background
#./explorer gds/09_tt_um_rejunity_atari2600.gds > /dev/null &
./fetsim gds/09_tt_um_znah_vga_ca.txt > /dev/null &
PID=$!
sleep 1

echo "Sampling PID $PID for 5 seconds..."
# Sample for 5 seconds
sample $PID 5 -file profile.out

# Clean up
kill $PID 2>/dev/null || true

echo "Sample complete. Results in profile.out"
echo "Top functions by self time:"
# Extract top of stack sorted by count
grep -A 20 "Sort by top of stack" profile.out #|| head -n 20 profile.out
