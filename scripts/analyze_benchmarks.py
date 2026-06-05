# Local ChameleonRT copy of the RPTR benchmark analysis script.
#
# Copied from rptr/scripts/analyze_benchmarks.py. The analysis logic is backend
# agnostic: it consumes the benchmark.csv / benchmark.json / scene_info.json
# artifacts produced by capture_benchmarks.py, which ChameleonRT now emits in the
# same schema. The original RPTR script is left untouched; only this local copy
# lives in the ChameleonRT tree.
#
# Usage:
#   python analyze_benchmarks.py <REPORT_CONFIG.JSON> <BENCHMARK_DIR_NAME> <REPORT_DIR_NAME>
import json
import math
import subprocess
import shutil
import sys
import os
import time
import copy
import pandas as pd
from pathlib import Path
from pandas.plotting import register_matplotlib_converters
import chart_studio.plotly as py
import plotly.tools as plotly_tools
import plotly.graph_objs as go
import plotly.io as pio
import plotly.express as px
from plotly.subplots import make_subplots
import plotly.figure_factory as ff
import datetime

register_matplotlib_converters()

SCRIPT_DIR = Path(__file__).parent

if len(sys.argv) < 4:
    print(f"usage: {sys.argv[0]} <REPORT_CONFIG.JSON> <BENCHMARK_DIR_NAME> <REPORT_DIR_NAME>")
    sys.exit(1)
    
config_file_name = sys.argv[1]
benchmark_dir_name = sys.argv[2]
report_dir_name = sys.argv[3]

with open(config_file_name) as json_file:
    report_config = json.load(json_file)
    
benchmark_root_dir = Path(benchmark_dir_name)

report_dir = Path(report_dir_name)
report_dir.mkdir(parents=True, exist_ok=True)

#scene info
with open(str(benchmark_root_dir / Path("scene_info.json"))) as json_file:
    scene_info = json.load(json_file)["info"]

try:
    with open(str(benchmark_root_dir / Path("benchmark_info.json"))) as json_file:
        benchmark_title = json.load(json_file)["title"]
except:
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    benchmark_title = f"Benchmark ({timestamp})"

system_infos = []
launch_infos = []

# load benchmarks as data frames
benchmark_dataframes = []
benchmark_names = []
benchmark_descs = []
benchmark_indices = [] #used to have a fixed color for different reports
plot_subset = []
benchmark_name_LUT = {}
ignore_frames_count = 0
smoothing_window_size = 5
report_title = "Benchmark Report"
report_description = ""

if "ignore_frames" in report_config:
    ignore_frames_count = report_config["ignore_frames"]
if "smoothing_window_size" in report_config:
    smoothing_window_size = report_config["smoothing_window_size"]
if "plot_subset" in report_config:
    plot_subset = report_config["plot_subset"]
if "title" in report_config:
    report_title = report_config["title"]
if "description" in report_config:
    report_description = report_config["description"]
    
# # representative frames

if "dump_frames_dir" in report_config:
    dump_frames_dir = benchmark_root_dir / Path(report_config["dump_frames_dir"])
else:
    dump_frames_dir = benchmark_root_dir / Path("dump_frames")

if len(str(dump_frames_dir)):
    print("Specified Dump Frames Dir : " + str(dump_frames_dir))


sample_frame_imgs = []
sample_frame_indices = []

# Check if the dump_frames directory exists
if dump_frames_dir.exists():
    
    frame_file_list = list(dump_frames_dir.glob('*.jpg')) + list(dump_frames_dir.glob('*.png'))

    num_dump_frames = min(5, len(frame_file_list))

    frame_idx_stride = int(len(frame_file_list) / num_dump_frames)

    for i in range(0, num_dump_frames):
        sample_frame_idx = min(i * frame_idx_stride, len(frame_file_list) - 1)
        sample_frame_imgs.append(frame_file_list[sample_frame_idx])
        sample_frame_indices.append(sample_frame_idx)
    
    #we move the sample images to the report root dir to make the HTML files portable
    report_img_root = report_dir / Path("sample_frames")
    report_img_root.mkdir(parents=True, exist_ok=True)
    for img_path in sample_frame_imgs:
        dst_path = report_img_root / Path(img_path.name)
        shutil.copy(img_path, dst_path)
else:
    print ("warning : failed to find dump frames dir!  Will be omitted from report")

benchmark_idx = 0
first_non_baseline_idx = 0  # Initialize to None to indicate no non-baseline directory found yet

for benchmark_dir in benchmark_root_dir.iterdir():
    if benchmark_dir.is_dir():
        
        is_baseline = benchmark_dir.stem.startswith("baseline")
        
        # Load render environment info
        benchmark_json_path = benchmark_dir / Path("benchmark.json")
        if benchmark_json_path.exists():
            print(str(benchmark_json_path))
            with open(str(benchmark_json_path)) as json_file:
                json_benchmark = json.load(json_file)
                system_infos.append(json_benchmark.get("system", "Baseline"))
                launch_infos.append(json_benchmark.get("launch", "Baseline"))
        else:
            # Append default values if benchmark.json is missing
            system_infos.append("Baseline")
            launch_infos.append("Baseline")

        if benchmark_dir.stem == "dump_frames":
            continue

        # Skip data if not selected for this current report
        if len(plot_subset) > 0 and not benchmark_dir.stem in plot_subset:
            continue

        # Fetch benchmark description from capture metadata
        benchmark_meta_path = benchmark_dir / Path("benchmark_meta.json")
        if benchmark_meta_path.exists():
            with open(str(benchmark_meta_path)) as json_file:
                benchmark_meta = json.load(json_file)
                benchmark_desc = benchmark_meta.get("desc", "Baseline")
        else:
            # Append default description if benchmark_meta.json is missing
            benchmark_desc = "Baseline"

        benchmark_descs.append(benchmark_desc)
        
        benchmark_name_LUT[benchmark_dir.stem] = len(benchmark_names)
        benchmark_names.append(benchmark_dir.stem)
        benchmark_indices.append(benchmark_idx)
        benchmark_df = pd.read_csv(benchmark_dir / "benchmark.csv")
        if ignore_frames_count > 0:
            benchmark_df = benchmark_df.drop(benchmark_df.index[0:ignore_frames_count])
        benchmark_dataframes.append(benchmark_df)

        if not is_baseline and first_non_baseline_idx == 0:
            first_non_baseline_idx = benchmark_idx  # Set to current index

        benchmark_idx += 1

#print(benchmark_dataframes[0])
plot_files = []
mean_files = []
data_counts = []
color_sequence = px.colors.qualitative.Dark24

for plot_dir in plot_subset:
    if not plot_dir in benchmark_names:
        print(f"Error: the benchmark dir '{plot_dir}' defined in the field 'plot_subset' is not found. Aborting...")
        exit(-1)

for plot_idx, report_plot in enumerate(report_config["generate_plots"]):
    col_name = report_plot["perf_metric"]
    display_name = col_name
    unit_name = " (ms)"
    if "alias" in report_plot:
        display_name = report_plot["alias"]
        
    mean_names = []
    mean_values = []
    data_colors = []
    data_count = 0
        
    #generate plot HTML
    print(f"Generating Plot: {display_name}")
    fig = go.Figure()
    if report_plot["plot_type"] == "standard":
        for i in range(len(benchmark_names)):
            try:
                data_col = benchmark_dataframes[i][col_name]
                data_count += 1
                mean_names.append(benchmark_names[i])
                mean_values.append(data_col.mean())
                data_colors.append(color_sequence[benchmark_indices[i]])
                data_ewm = data_col.ewm(span = smoothing_window_size, adjust=False).mean()
                fig.add_trace(go.Scatter(x = benchmark_dataframes[i]["frames_total"], y = data_ewm, mode = "lines", line = dict(width=3, color = color_sequence[benchmark_indices[i]]), name = benchmark_names[i]))
            except KeyError as e:
                print(f"Skipping chart for {benchmark_names[i]} due to missing column: {e}")
            except Exception as e:
                print(f"An error occurred while processing {benchmark_names[i]}: {e}")

    elif report_plot["plot_type"] == "relative":
        baseline_col_idx = benchmark_name_LUT[report_plot["baseline"]]

        try:
            baseline_col = benchmark_dataframes[baseline_col_idx][col_name]
        except KeyError as e:
            print(f"Relative plot : Skipping chart for {col_name} due to missing column: {e}")
            continue
        except Exception as e:
            print(f"Relative plot : An error occurred while processing {col_name}: {e}")
            continue

        baseline_ewm = baseline_col.ewm(span = smoothing_window_size, adjust=False).mean()
        unit_name = f" relative to {report_plot['baseline']} (%)"
        for i in range(len(benchmark_names)):
            if i == baseline_col_idx:
                continue
            data_col = benchmark_dataframes[i][col_name]
            data_colors.append(color_sequence[benchmark_indices[i]])
            data_count += 1
            data_ewm = data_col.ewm(span = smoothing_window_size, adjust=False).mean()
            data_rel = data_ewm / baseline_ewm
            mean_names.append(benchmark_names[i])
            mean_values.append((data_col/baseline_col).mean() * 100.0)
            fig.add_trace(go.Scatter(x = benchmark_dataframes[i]["frames_total"], y = data_rel, mode = "lines", line = dict(width=3, color = color_sequence[benchmark_indices[i]]), name = benchmark_names[i]))
        fig.update_layout(yaxis_tickformat = '.2%')

    if "constant" in report_plot:
        fig.add_trace(go.Scatter(
            x=[None], y=[None], # No actual data points
            mode='markers', 
            marker=dict(color=color_sequence[len(benchmark_indices) + 1], size=10),
            name=report_plot["constant-legend"],
            showlegend=True
        ))      
        fig.add_shape(go.layout.Shape(type="line", x0=0, y0=report_plot["constant"], x1=len(fig.data[0].x), y1=report_plot["constant"], line=dict(color=color_sequence[len(benchmark_indices) + 1], width=3)))
    
    fig.update_layout(hovermode='x unified')
    fig.update_layout(title_text = display_name + unit_name)
    fig.update_layout(legend=dict(
        yanchor="top",
        y=0.99,
        xanchor="left",
        x=0.01
    ))
    plot_file = report_dir / f"benchmark_plot_{plot_idx}.html"
    plot_files.append(plot_file)
    pio.write_html(fig, str(plot_file), auto_open = False)
    
    #generate bar chart HTML for mean values
    #print(mean_values)
    fig2 = go.Figure([go.Bar(x = mean_names, y = mean_values, marker_color = data_colors, width = 0.75)])
    fig2.update_layout(title_text = "Mean values " + unit_name)
    mean_file = report_dir / f"benchmark_means_{plot_idx}.html"
    mean_files.append(mean_file)
    pio.write_html(fig2, str(mean_file), auto_open = False)
    
    data_counts.append(data_count)

sample_frames_string = '''
<table><tbody><tr>'''

for i in range(len(sample_frame_imgs)):
    sample_frames_string += '''
    <td><div class="sampleFrame">
    <img src="sample_frames/'''+str(sample_frame_imgs[i].name)+'''"></img>
    <p>''' + str(sample_frame_indices[i]) + '''</p>
</div></td>
'''

sample_frames_string += '''
</tr></tbody></table>
'''

plots_html_string = '''
<table width=100%><tbody>
'''

title = f"{report_title} ({benchmark_title})"

for i in range(len(plot_files)):
    plots_html_string += '''
        <tr>
        <td>
            <div class="issuu-embed-container" style="height:500px">
                <iframe frameborder="0" seamless="seamless" scrolling="no" allowfullscreen src="''' + plot_files[i].name + '''"></iframe>
            </div>
        </td>
        <td width="10%">
            <div class="issuu-embed-container" style="width:200px;height:500px">
                <iframe frameborder="0" seamless="seamless" scrolling="no" allowfullscreen src="''' + mean_files[i].name + '''"></iframe>
            </div>
        </td>
        </tr>
            
    '''
    
plots_html_string += '''
</tbody></table>
'''

benchmark_desc_html = '''
<ul>
'''
for i in range(len(benchmark_names)):
    benchmark_desc_html += f"<li><b>{benchmark_names[i]}</b>: {benchmark_descs[i]}</li>"
benchmark_desc_html += '''
</ul>
'''

#technically each benchmark might have had different resolutions or other display settings. However, we will only display one of them for now
system_info = system_infos[first_non_baseline_idx]
launch_info = launch_infos[first_non_baseline_idx]
summary_html = '''
<table class="summaryTable">
<tbody>
<tr>
<td>
<h3>Scene</h3>
<table class="greyGridTable">
<tbody>
<tr>
<td width="150px"><b>unique tris</b></td><td>''' + str(scene_info["unique_tris"]) + '''</td></tr>
<tr>
<td><b>total tris</b></td><td>''' + str(scene_info["total_tris"]) + '''</td></tr>
<tr>
<td><b>param meshes</b></td><td>''' + str(scene_info["num_param_meshes"]) + '''</td></tr>
<tr>
<td><b>instances</b></td><td>''' + str(scene_info["num_instances"]) + '''</td></tr>
<tr>
<td><b>lod groups</b></td><td>''' + str(scene_info["num_lod_groups"]) + '''</td></tr>
</tbody>
</tr>
</table>
</td>
<td>
<h3>Render Environment</h3>
<table class="greyGridTable">
<tbody>
<tr>
<td width="100px"><b>CPU</b></td><td>''' + str(system_info["cpu"]) + '''</td></tr>
<tr>
<td><b>GPU</b></td><td>''' + str(system_info["gpu"]) + '''</td></tr>
<tr>
<td><b>Display</b></td><td>''' + str(system_info["display"]) + '''</td></tr>
<tr>
<td><b>Display Res</b></td><td>''' + f"{launch_info['display_res'][0]} x {launch_info['display_res'][1]}" + '''</td></tr>
<tr>
<td><b>Render Res</b></td><td>''' + f"{launch_info['render_res'][0]} x {launch_info['render_res'][1]}" + '''</td></tr>
</tbody>
</tr>
</table>
</td>
</tr>
</tbody>
</table>'''

html_string = '''
    <html>
        <head>
            <title>''' + title + '''</title>
            <link rel="stylesheet" href="https://maxcdn.bootstrapcdn.com/bootstrap/3.3.1/css/bootstrap.min.css">
    <style>

    body{ margin:0 0; background:white; }

    table.summaryTable {
    }
    table.summaryTable td, table.greyGridTable th {
      padding-right: 40px;
    }
    
    table.greyGridTable {
      border: 2px solid #FFFFFF;
      width: 400px;
      text-align: left;
      border-collapse: collapse;
    }
    table.greyGridTable td, table.greyGridTable th {
      border: 1px solid #FFFFFF;
      padding: 3px 4px;
    }
    table.greyGridTable tbody td {
      font-size: 13px;
    }
    table.greyGridTable tr:nth-child(even) {
      background: #D0E4F5;
    }
    table.greyGridTable tfoot td {
      font-size: 14px;
    }
    .sampleFrame{
        width:300px;
        float:center;
        margin:5px;
    }
    .sampleFrame img{
        width:100%;
    }

    .issuu-embed-container {

    position: relative;

    padding-bottom: 56.25%; /* set the aspect ratio here as (height / width) * 100% */

    height: 0;

    overflow: hidden;

    max-width: 100%;

    }

    .issuu-embed-container iframe {

    position: absolute;

    top: 0;

    left: 0;

    width: 100%;

    height: 100%;

    }

    </style>
    </head>
        <body>
            <h1>''' + title + ''' </h1>

            <h2>Summary</h2>
            
            <p>''' + report_description + '''</p>

            ''' + benchmark_desc_html + ''' 

            ''' + summary_html + '''

            <h2>Sample Frames</h2>
            
            ''' + sample_frames_string + '''

            <h2>Plots</h2>
            
            ''' + plots_html_string + '''
            
            
        </body>
    </html>'''

benchmark_report_file = str(report_dir / "benchmark_report.html")
f = open(benchmark_report_file, "w")
f.write(html_string)
f.close()

print("Report done!")
#os.system(benchmark_report_file)