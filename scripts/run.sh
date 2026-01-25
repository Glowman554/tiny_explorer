#!/bin/bash
# Batch process GDS files and generate a report
# Run from root: ./scripts/run.sh

set -e

# Get the script's directory and project root
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
ROOT="$DIR/.."

# Build the native parser
echo "Compiling native parse_gds..."
g++ -std=c++17 -O3 "$ROOT/src/parse_gds.cpp" -o "$ROOT/parse_gds"

# Build the WASM parser
"$ROOT/scripts/build_wasm.sh"

# Collect all GDS files
GDS_FILES=$(find gds -name "*.gds" -o -name "*.gds.br" | sort)
COUNT=$(echo "$GDS_FILES" | wc -l | xargs)

echo "Processing $COUNT GDS files in parallel..."

# Temporary directory for logs
mkdir -p "$ROOT/logs"
rm -f "$ROOT/logs"/*.log

# Function to run the parser and capture output
run_parser() {
    local gds_file=$1
    local log_file=$2
    "$ROOT/parse_gds" "$gds_file" > "$log_file" 2>&1
}

# Run in parallel (backgrounding)
for f in $GDS_FILES; do
    log_name=$(echo "$f" | sed 's/\//_/g').log
    run_parser "$f" "$ROOT/logs/$log_name" &
done

# Wait for all background processes to finish
wait

echo "Processing complete. Generating report.html..."

# Start generating report.html in the root
cat <<EOF > "$ROOT/report.html"
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>GDS Processing Report</title>
    <style>
        :root {
            --bg: #0f172a;
            --surface: #1e293b;
            --primary: #38bdf8;
            --text: #f8fafc;
            --text-dim: #94a3b8;
            --border: #334155;
            --success: #22c55e;
            --error: #ef4444;
        }
        body {
            background-color: var(--bg);
            color: var(--text);
            font-family: -apple-system, sans-serif;
            margin: 0;
            padding: 40px;
        }
        .container { max-width: 1200px; margin: 0 auto; }
        header { margin-bottom: 2rem; border-bottom: 1px solid var(--border); padding-bottom: 1rem; }
        h1 { color: var(--primary); margin: 0; }
        .subtitle { color: var(--text-dim); }
        table { width: 100%; border-collapse: separate; border-spacing: 0; background: var(--surface); border-radius: 12px; border: 1px solid var(--border); }
        th { text-align: left; padding: 12px 20px; color: var(--text-dim); font-size: 0.75rem; text-transform: uppercase; border-bottom: 1px solid var(--border); cursor: pointer; }
        td { padding: 12px 20px; border-bottom: 1px solid var(--border); font-size: 0.9rem; }
        .file-column { font-family: monospace; color: #cbd5e1; }
        .time-column { color: var(--primary); }
        .status-badge { padding: 4px 10px; border-radius: 20px; font-size: 0.7rem; font-weight: bold; background: rgba(34, 197, 94, 0.1); color: var(--success); }
        .status-badge.error { background: rgba(239, 68, 68, 0.1); color: var(--error); }
        .btn-action { display: inline-block; padding: 4px 10px; border-radius: 4px; font-weight: bold; font-size: 0.75rem; text-decoration: none; margin-right: 4px; }
        .btn-view { background: var(--primary); color: var(--bg); }
        .btn-log { background: rgba(255,255,255,0.1); color: #fff; border: 1px solid var(--border); }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>GDS Processing Report</h1>
            <p class="subtitle">Automated GDS parsing results (Generated: $(date))</p>
        </header>
        <table id="resultsTable">
            <thead>
                <tr>
                    <th onclick="sortTable(0)">GDS File</th>
                    <th onclick="sortTable(1)">Rects</th>
                    <th onclick="sortTable(2)">Wires</th>
                    <th onclick="sortTable(3)">FETs</th>
                    <th onclick="sortTable(4)">Time</th>
                    <th onclick="sortTable(5)">Warn</th>
                    <th onclick="sortTable(6)">Err</th>
                    <th>Status</th>
                    <th>Actions</th>
                </tr>
            </thead>
            <tbody>
EOF

# Row iteration
for f in $GDS_FILES; do
    log_name=$(echo "$f" | sed 's/\//_/g').log
    log_file="$ROOT/logs/$log_name"
    
    # Extract stats
    flat_rects_raw=$(grep "Stats:.*FlatRects=" "$log_file" | sed -E 's/.*FlatRects=([0-9]+).*/\1/' || echo "0")
    flat_fets_raw=$(grep "Stats:.*FlatFETs=" "$log_file" | sed -E 's/.*FlatFETs=([0-9]+).*/\1/' || echo "0")
    flat_wires_raw=$(grep "Stats:.*NetlistWires=" "$log_file" | sed -E 's/.*NetlistWires=([0-9]+).*/\1/' || echo "0")
    warn_raw=$(grep -c "Warning:" "$log_file" || true)
    err_raw=$(grep -c "Error:" "$log_file" || true)
    
    # Extract times and sum them
    perf_line=$(grep "Performance:" "$log_file" || true)
    parse_ms=$(echo "$perf_line" | sed -E 's/.*GDS Parse: +([0-9.]+).*/\1/' | head -n 1)
    export_ms=$(echo "$perf_line" | sed -E 's/.*Export \(Netlist\): +([0-9.]+).*/\1/' | head -n 1)
    
    total_ms_disp="N/A"
    status="OK"
    status_class=""
    
    if [ -n "$parse_ms" ]; then
        # Use python for floating point addition if available, otherwise just use parse time
        total_ms=$(python3 -c "print(f'{float(\"${parse_ms:-0}\") + float(\"${export_ms:-0}\"):.2f}')" 2>/dev/null || echo "$parse_ms")
        total_ms_disp="${total_ms}ms"
    else
        status="Crash"
        status_class="error"
        flat_rects_disp="-"
    fi

    if [ "$err_raw" -gt 0 ]; then
        status="Error ($err_raw)"
        status_class="error"
    fi

    # Clean up file name for display
    shuttle_name=$(echo "$f" | cut -d'/' -f2)
    display_name=$(basename "$f")
    display_name=${display_name%.br}
    display_name=${display_name%.gds}

    cat <<EOF >> "$ROOT/report.html"
                <tr>
                    <td class="file-column">
                        <span style="opacity: 0.5; font-size: 0.7rem; display: block;">$shuttle_name</span>
                        <a href="viewer.html?file=$f" target="_blank" style="text-decoration: none; font-weight: 600; color: var(--primary);">
                            $display_name
                        </a>
                    </td>
                    <td>$flat_rects_raw</td>
                    <td>$flat_wires_raw</td>
                    <td>$flat_fets_raw</td>
                    <td class="time-column">$total_ms_disp</td>
                    <td>$warn_raw</td>
                    <td>$err_raw</td>
                    <td><span class="status-badge $status_class">$status</span></td>
                    <td>
                        <a href="viewer.html?file=$f" class="btn-action btn-view" target="_blank">View</a>
                        <a href="logs/$log_name" class="btn-action btn-log" target="_blank">Logs</a>
                    </td>
                </tr>
EOF
done

cat <<EOF >> "$ROOT/report.html"
            </tbody>
        </table>
    </div>
    <script>
        function sortTable(n) {
            const table = document.getElementById("resultsTable");
            const tbody = table.querySelector("tbody");
            const rows = Array.from(tbody.rows);
            const dir = table.getAttribute("data-sort-dir") === "asc" ? "desc" : "asc";
            table.setAttribute("data-sort-dir", dir);
            rows.sort((a, b) => {
                let x = a.cells[n].innerText.replace(/ms|,/g, "");
                let y = b.cells[n].innerText.replace(/ms|,/g, "");
                const nx = parseFloat(x), ny = parseFloat(y);
                if (!isNaN(nx) && !isNaN(ny)) { x = nx; y = ny; }
                if (x < y) return dir === "asc" ? -1 : 1;
                if (x > y) return dir === "asc" ? 1 : -1;
                return 0;
            });
            rows.forEach(row => tbody.appendChild(row));
        }
    </script>
</body>
</html>
EOF

echo "Done! Report generated at report.html"