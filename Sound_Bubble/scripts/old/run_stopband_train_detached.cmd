@echo off
setlocal
cd /d C:\Users\darre\Sound_Bubble_optim_distance
set PYTHONPATH=C:\Users\darre\Sound_Bubble_optim_distance
set LOG_PATH=C:\Users\darre\Sound_Bubble_optim_distance\runs\count_distance_head_optim_1_5m_stopband\train_stopband_detached.log
set STATUS_PATH=C:\Users\darre\Sound_Bubble_optim_distance\runs\count_distance_head_optim_1_5m_stopband\train_stopband_detached.exit.txt
if exist "%STATUS_PATH%" del /f /q "%STATUS_PATH%"
"C:\Users\darre\AppData\Local\Programs\Python\Python311\python.exe" -u -m src.train_pt --config real_experiments/count_distance_head_optim_1_5m_stopband.json --run_dir runs/count_distance_head_optim_1_5m_stopband --no_wandb > "%LOG_PATH%" 2>&1
echo %errorlevel% > "%STATUS_PATH%"
