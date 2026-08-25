function results = analyze_openvlc_analog_csv(csvPath, varargin)
%ANALYZE_OPENVLC_ANALOG_CSV Analyze OpenVLC AGC/comparator oscilloscope CSVs.
%
% Usage:
%   results = analyze_openvlc_analog_csv();
%   results = analyze_openvlc_analog_csv("C:\Users\lucas\Downloads\analog.csv");
%   results = analyze_openvlc_analog_csv("analog.csv", ...
%       "ThresholdsV", 1.60:0.025:2.10, ...
%       "DefaultThresholdV", 1.800, ...
%       "ExportDir", "C:\Users\lucas\Downloads\openvlc_analysis");
%
% What it does:
%   1. Reads a two-column scope CSV: time [s], voltage [V].
%   2. Computes amplitude/noise/sample-rate statistics.
%   3. Sweeps comparator thresholds in volts.
%   4. Reconstructs threshold crossings with linear interpolation.
%   5. Applies STM32-like short-pulse cancellation.
%   6. Groups edge bursts using the configured idle gap.
%   7. Reports run-length quality: 1-cell, 2-cell, 3-cell, long/impossible.
%   8. Recommends threshold/DAC/mV values for openvlc_board.h.
%   9. Shows what the STM32 comparator/TIM2 path sees with the configured
%      threshold, hysteresis, deglitch and burst-gap parameters.
%  10. Produces diagnostic plots.
%
% The goal is not to fully decode OpenVLC frames in MATLAB. The goal is to
% answer the important hardware/software question first: "does this analog
% waveform produce a comparator edge stream that the STM32 decoder can trust?"

if nargin < 1 || strlength(string(csvPath)) == 0
    csvPath = "C:\Users\lucas\Downloads\analog.csv";
end

cfg = defaultConfig();
cfg = parseNameValue(cfg, varargin{:});

csvPath = string(csvPath);
if ~isfile(csvPath)
    error("CSV file not found: %s", csvPath);
end

[time_s, voltage_v] = readScopeCsv(csvPath);
if numel(time_s) < 4
    error("CSV must contain at least four numeric samples.");
end

summary = summarizeWaveform(time_s, voltage_v);
sweep = sweepThresholds(time_s, voltage_v, cfg);

if isempty(sweep)
    error("No usable threshold result. Check ThresholdsV and CSV contents.");
end

[~, bestIdx] = max([sweep.score]);
best = sweep(bestIdx);

defaultStats = analyzeThreshold(time_s, voltage_v, cfg.DefaultThresholdV, cfg);
defaultComparator = simulateComparatorView(time_s, voltage_v, cfg.DefaultThresholdV, cfg);
bestComparator = simulateComparatorView(time_s, voltage_v, best.threshold_v, cfg);

if cfg.Verbose
    printSummary(csvPath, summary, cfg, best, defaultStats, defaultComparator, sweep);
end

figures = [];
if cfg.Plot
    figures = makePlots(time_s, voltage_v, cfg, summary, sweep, best, defaultStats, defaultComparator);
end

if strlength(string(cfg.ExportDir)) > 0
    exportAnalysis(cfg.ExportDir, csvPath, cfg, summary, sweep, best, defaultStats, defaultComparator, figures);
end

results = struct();
results.csvPath = csvPath;
results.config = cfg;
results.summary = summary;
results.sweep = sweepToTable(sweep);
results.best = best;
results.defaultThreshold = defaultStats;
results.defaultComparator = defaultComparator;
results.bestComparator = bestComparator;
results.figures = figures;
end

function cfg = defaultConfig()
cfg = struct();

% STM32/Pi HAT defaults. Keep these aligned with Core/Inc/openvlc_board.h.
cfg.VrefMV = 3300;
cfg.TimerHz = 64e6;
cfg.MinIntervalTicks = 14;      % OPENVLC_EDGE_MIN_INTERVAL_TICKS
cfg.GapUs = 4.0;                % OPENVLC_EDGE_GAP_US
cfg.Run1MaxTicks = 52;          % roughly <1.6 cells at budget 50
cfg.Run2MaxTicks = 88;
cfg.Run3MaxTicks = 120;
cfg.FullBurstEdges = [10000 13000]; % 828-byte TUN packet at profile 1000
cfg.ComparatorHysteresisMV = 0;  % LOW hardware hysteresis; exact mV not modelled

% Sweep defaults. Adjust these when the AGC mean moves.
cfg.ThresholdsV = 1.30:0.025:2.30;
cfg.DefaultThresholdV = 1.500;

% Heuristic decode quality classes, calibrated from STM32 logs:
%   good:     similar to okps~=seenps logs, long roughly below 80/burst.
%   marginal: might decode with RS/repair, but expect packet loss.
%   bad:      comparator stream already lost too many transitions.
cfg.DecodeGoodLongMax = 80;
cfg.DecodeMarginalLongMax = 160;
cfg.DecodeGoodLmaxUs = 2.5;
cfg.DecodeMarginalLmaxUs = 3.5;

% Scoring weights. Higher score means "more likely to be decodable".
cfg.ScoreFullBurstWeight = 10000;
cfg.ScoreLongRunPenalty = 40;
cfg.ScoreEdgeDistancePenalty = 0.2;
cfg.ScoreRemovedPulsePenalty = 0.02;
cfg.ScoreDutyPenalty = 2.0;
cfg.ExpectedFullBurstEdges = 11800;
cfg.TargetDutyPermille = 500;

cfg.Plot = true;
cfg.MaxWaveformPoints = 120000;
cfg.MaxBurstPlots = 3;
cfg.ComparatorViewWindowMs = 5.0;
cfg.ExportDir = "";
cfg.Verbose = true;
end

function cfg = parseNameValue(cfg, varargin)
if mod(numel(varargin), 2) ~= 0
    error("Name-value arguments must be pairs.");
end

for k = 1:2:numel(varargin)
    name = char(varargin{k});
    value = varargin{k + 1};
    if ~isfield(cfg, name)
        error("Unknown option '%s'.", name);
    end
    cfg.(name) = value;
end
end

function [time_s, voltage_v] = readScopeCsv(csvPath)
raw = readmatrix(csvPath);
if size(raw, 2) < 2
    error("CSV must contain at least two numeric columns: time, voltage.");
end

time_s = raw(:, 1);
voltage_v = raw(:, 2);
valid = isfinite(time_s) & isfinite(voltage_v);
time_s = time_s(valid);
voltage_v = voltage_v(valid);

% Scope exports sometimes contain absolute timestamps. Use relative time for
% plots and burst durations while preserving the sample spacing.
time_s = time_s - time_s(1);
end

function summary = summarizeWaveform(time_s, voltage_v)
dt = median(diff(time_s));
dv = diff(voltage_v);

summary = struct();
summary.samples = numel(voltage_v);
summary.duration_s = time_s(end) - time_s(1);
summary.dt_s = dt;
summary.sample_rate_hz = 1 / dt;
summary.min_v = min(voltage_v);
summary.max_v = max(voltage_v);
summary.mean_v = mean(voltage_v);
summary.std_v = std(voltage_v);
summary.pp_v = summary.max_v - summary.min_v;
summary.diff_std_mv = std(dv) * 1000;
summary.diff_mad_mv = madLocal(dv) * 1000;
summary.percentiles_v = percentileLocal(voltage_v, [0.1 1 5 10 25 50 75 90 95 99 99.9]);
summary.percentile_labels = [0.1 1 5 10 25 50 75 90 95 99 99.9];
end

function sweep = sweepThresholds(time_s, voltage_v, cfg)
thresholds = cfg.ThresholdsV(:)';
sweep = repmat(emptyThresholdStats(), 1, numel(thresholds));

for i = 1:numel(thresholds)
    sweep(i) = analyzeThreshold(time_s, voltage_v, thresholds(i), cfg);
end

% Remove thresholds that generated no edges at all.
keep = [sweep.raw_edges] > 0;
sweep = sweep(keep);
end

function stats = analyzeThreshold(time_s, voltage_v, threshold_v, cfg)
stats = emptyThresholdStats();
stats.threshold_v = threshold_v;
stats.threshold_mv = round(threshold_v * 1000);
stats.threshold_dac = mvToDac(stats.threshold_mv, cfg.VrefMV);

rawEdges = thresholdCrossingsComparator(time_s, voltage_v, threshold_v, cfg);
stats.raw_edges = numel(rawEdges);
if numel(rawEdges) < 2
    return;
end

minIntervalS = cfg.MinIntervalTicks / cfg.TimerHz;
[edges, removed] = cancelShortPulses(rawEdges, minIntervalS);
stats.edges = numel(edges);
stats.removed_edges = removed;
if numel(edges) < 2
    return;
end

stats.all_duty_permille = mean(voltage_v > threshold_v) * 1000;

[burstStart, burstEnd] = splitBursts(edges, cfg.GapUs * 1e-6);
burstLens = burstEnd - burstStart + 1;
stats.bursts = numel(burstLens);
stats.max_burst_edges = max(burstLens);

fullMask = burstLens >= cfg.FullBurstEdges(1) & burstLens <= cfg.FullBurstEdges(2);
fullStart = burstStart(fullMask);
fullEnd = burstEnd(fullMask);
stats.full_bursts = numel(fullStart);

if stats.full_bursts == 0
    stats.score = -4e6 - stats.removed_edges * cfg.ScoreRemovedPulsePenalty;
    stats.top_burst_edges = topN(burstLens, 8);
    return;
end

burstStats = repmat(emptyBurstStats(), stats.full_bursts, 1);
for i = 1:stats.full_bursts
    burstStats(i) = analyzeBurst(edges, fullStart(i), fullEnd(i), time_s, voltage_v, threshold_v, cfg);
end

stats.top_burst_edges = topN(burstLens, 8);
stats.median_full_edges = median([burstStats.edges]);
stats.median_duration_ms = median([burstStats.duration_ms]);
stats.median_r1 = median([burstStats.r1]);
stats.median_r2 = median([burstStats.r2]);
stats.median_r3 = median([burstStats.r3]);
stats.median_long = median([burstStats.long]);
stats.median_lmax_ticks = median([burstStats.lmax_ticks]);
stats.median_lmax_us = median([burstStats.lmax_us]);
stats.median_interval_us = median([burstStats.median_interval_us]);
stats.median_duty_permille = median([burstStats.duty_permille]);
stats.first_full_burst = burstStats(1);

stats.score = ...
    stats.full_bursts * cfg.ScoreFullBurstWeight ...
    - stats.median_long * cfg.ScoreLongRunPenalty ...
    - abs(stats.median_full_edges - cfg.ExpectedFullBurstEdges) * cfg.ScoreEdgeDistancePenalty ...
    - stats.removed_edges * cfg.ScoreRemovedPulsePenalty ...
    - abs(stats.median_duty_permille - cfg.TargetDutyPermille) * cfg.ScoreDutyPenalty;
end

function comp = simulateComparatorView(time_s, voltage_v, threshold_v, cfg)
% Simulate the STM32 COMP1 -> TIM2_CH4 -> deglitch -> burst grouping path.
comp = struct();
comp.threshold_v = threshold_v;
comp.threshold_mv = round(threshold_v * 1000);
comp.threshold_dac = mvToDac(comp.threshold_mv, cfg.VrefMV);
comp.hysteresis_mv = cfg.ComparatorHysteresisMV;
comp.upper_threshold_v = threshold_v + (cfg.ComparatorHysteresisMV / 2000);
comp.lower_threshold_v = threshold_v - (cfg.ComparatorHysteresisMV / 2000);
comp.min_interval_ticks = cfg.MinIntervalTicks;
comp.min_interval_us = cfg.MinIntervalTicks / cfg.TimerHz * 1e6;
comp.gap_us = cfg.GapUs;

rawEdges = thresholdCrossingsComparator(time_s, voltage_v, threshold_v, cfg);
[edges, removed] = cancelShortPulses(rawEdges, cfg.MinIntervalTicks / cfg.TimerHz);
[burstStart, burstEnd] = splitBursts(edges, cfg.GapUs * 1e-6);

comp.raw_edges = rawEdges;
comp.edges = edges;
comp.removed_edges = removed;
comp.burst_start = burstStart;
comp.burst_end = burstEnd;

burstTable = comparatorBurstTable(edges, burstStart, burstEnd, time_s, voltage_v, threshold_v, cfg);
comp.burstTable = burstTable;
comp.decodeEstimate = estimateDecodeFromBurstTable(burstTable, cfg);
end

function stats = emptyThresholdStats()
stats = struct();
stats.threshold_v = NaN;
stats.threshold_mv = NaN;
stats.threshold_dac = NaN;
stats.raw_edges = 0;
stats.edges = 0;
stats.removed_edges = 0;
stats.bursts = 0;
stats.full_bursts = 0;
stats.max_burst_edges = 0;
stats.top_burst_edges = [];
stats.all_duty_permille = NaN;
stats.median_full_edges = 0;
stats.median_duration_ms = 0;
stats.median_r1 = 0;
stats.median_r2 = 0;
stats.median_r3 = 0;
stats.median_long = 99999;
stats.median_lmax_ticks = 0;
stats.median_lmax_us = 0;
stats.median_interval_us = 0;
stats.median_duty_permille = NaN;
stats.first_full_burst = emptyBurstStats();
stats.score = -Inf;
end

function b = emptyBurstStats()
b = struct();
b.edges = 0;
b.duration_ms = 0;
b.r1 = 0;
b.r2 = 0;
b.r3 = 0;
b.long = 0;
b.lmax_ticks = 0;
b.lmax_us = 0;
b.median_interval_us = 0;
b.p01_interval_us = 0;
b.p99_interval_us = 0;
b.duty_permille = NaN;
end

function tbl = comparatorBurstTable(edges, burstStart, burstEnd, time_s, voltage_v, threshold_v, cfg)
if isempty(burstStart)
    tbl = table();
    return;
end

n = numel(burstStart);
burst_idx = (1:n)';
start_ms = zeros(n, 1);
end_ms = zeros(n, 1);
edges_count = zeros(n, 1);
duration_ms = zeros(n, 1);
r1 = zeros(n, 1);
r2 = zeros(n, 1);
r3 = zeros(n, 1);
long = zeros(n, 1);
lmax_us = zeros(n, 1);
median_interval_us = zeros(n, 1);
duty_permille = nan(n, 1);
is_full = false(n, 1);
decode_class = strings(n, 1);

for i = 1:n
    first = burstStart(i);
    last = burstEnd(i);
    bs = analyzeBurst(edges, first, last, time_s, voltage_v, threshold_v, cfg);

    start_ms(i) = edges(first) * 1000;
    end_ms(i) = edges(last) * 1000;
    edges_count(i) = bs.edges;
    duration_ms(i) = bs.duration_ms;
    r1(i) = bs.r1;
    r2(i) = bs.r2;
    r3(i) = bs.r3;
    long(i) = bs.long;
    lmax_us(i) = bs.lmax_us;
    median_interval_us(i) = bs.median_interval_us;
    duty_permille(i) = bs.duty_permille;
    is_full(i) = bs.edges >= cfg.FullBurstEdges(1) && bs.edges <= cfg.FullBurstEdges(2);
    decode_class(i) = classifyBurstDecode(bs, is_full(i), cfg);
end

tbl = table( ...
    burst_idx, start_ms, end_ms, edges_count, duration_ms, ...
    r1, r2, r3, long, lmax_us, median_interval_us, duty_permille, ...
    is_full, decode_class);
end

function cls = classifyBurstDecode(bs, isFull, cfg)
if ~isFull
    cls = "not_full";
elseif bs.long <= cfg.DecodeGoodLongMax && bs.lmax_us <= cfg.DecodeGoodLmaxUs
    cls = "good";
elseif bs.long <= cfg.DecodeMarginalLongMax && bs.lmax_us <= cfg.DecodeMarginalLmaxUs
    cls = "marginal";
else
    cls = "bad";
end
end

function est = estimateDecodeFromBurstTable(tbl, cfg)
est = struct();
if isempty(tbl)
    est.full_bursts = 0;
    est.good_bursts = 0;
    est.marginal_bursts = 0;
    est.bad_bursts = 0;
    est.estimated_clean_decode_pct = 0;
    est.estimated_possible_decode_pct = 0;
    est.median_full_long = NaN;
    est.median_full_lmax_us = NaN;
    est.note = "no bursts";
    return;
end

full = tbl.is_full;
est.full_bursts = sum(full);
est.good_bursts = sum(tbl.decode_class == "good");
est.marginal_bursts = sum(tbl.decode_class == "marginal");
est.bad_bursts = sum(tbl.decode_class == "bad");

if est.full_bursts > 0
    est.estimated_clean_decode_pct = 100 * est.good_bursts / est.full_bursts;
    est.estimated_possible_decode_pct = 100 * (est.good_bursts + est.marginal_bursts) / est.full_bursts;
    est.median_full_long = median(tbl.long(full));
    est.median_full_lmax_us = median(tbl.lmax_us(full));
else
    est.estimated_clean_decode_pct = 0;
    est.estimated_possible_decode_pct = 0;
    est.median_full_long = NaN;
    est.median_full_lmax_us = NaN;
end

if est.full_bursts == 0
    est.note = "no full-size packet bursts detected";
elseif est.good_bursts == est.full_bursts
    est.note = "edge stream looks clean";
elseif est.good_bursts + est.marginal_bursts > 0
    est.note = "some bursts may decode, but packet loss is expected";
else
    est.note = sprintf("all full bursts exceed long-run limits: good<=%d long, marginal<=%d long", ...
        cfg.DecodeGoodLongMax, cfg.DecodeMarginalLongMax);
end
end

function burst = analyzeBurst(edges, firstIdx, lastIdx, time_s, voltage_v, threshold_v, cfg)
burst = emptyBurstStats();
burst.edges = lastIdx - firstIdx + 1;
burst.duration_ms = (edges(lastIdx) - edges(firstIdx)) * 1000;
if burst.edges < 2
    inside = time_s >= edges(firstIdx) & time_s <= edges(lastIdx);
    if any(inside)
        burst.duty_permille = mean(voltage_v(inside) > threshold_v) * 1000;
    end
    return;
end

interval_s = diff(edges(firstIdx:lastIdx));
runTicks = interval_s * cfg.TimerHz;

burst.r1 = sum(runTicks < cfg.Run1MaxTicks);
burst.r2 = sum(runTicks >= cfg.Run1MaxTicks & runTicks < cfg.Run2MaxTicks);
burst.r3 = sum(runTicks >= cfg.Run2MaxTicks & runTicks < cfg.Run3MaxTicks);
burst.long = sum(runTicks >= cfg.Run3MaxTicks);
burst.lmax_ticks = max(runTicks);
burst.lmax_us = burst.lmax_ticks / cfg.TimerHz * 1e6;
burst.median_interval_us = median(interval_s) * 1e6;
burst.p01_interval_us = percentileLocal(interval_s * 1e6, 1);
burst.p99_interval_us = percentileLocal(interval_s * 1e6, 99);

inside = time_s >= edges(firstIdx) & time_s <= edges(lastIdx);
if any(inside)
    burst.duty_permille = mean(voltage_v(inside) > threshold_v) * 1000;
end
end

function edges = thresholdCrossingsComparator(time_s, voltage_v, threshold_v, cfg)
% Comparator model. With ComparatorHysteresisMV=0 this is a normal threshold
% crossing. With hysteresis enabled, threshold_v is the centre of the band:
% LOW->HIGH at threshold+hyst/2, HIGH->LOW at threshold-hyst/2.
if cfg.ComparatorHysteresisMV <= 0
    edges = thresholdCrossings(time_s, voltage_v, threshold_v);
    return;
end

upper = threshold_v + cfg.ComparatorHysteresisMV / 2000;
lower = threshold_v - cfg.ComparatorHysteresisMV / 2000;
stateHigh = voltage_v(1) > threshold_v;
edges = zeros(numel(voltage_v), 1);
count = 0;

for i = 2:numel(voltage_v)
    if stateHigh
        if voltage_v(i) < lower
            count = count + 1;
            edges(count) = interpolateCrossing(time_s(i - 1), voltage_v(i - 1), ...
                time_s(i), voltage_v(i), lower);
            stateHigh = false;
        end
    else
        if voltage_v(i) > upper
            count = count + 1;
            edges(count) = interpolateCrossing(time_s(i - 1), voltage_v(i - 1), ...
                time_s(i), voltage_v(i), upper);
            stateHigh = true;
        end
    end
end

edges = edges(1:count);
end

function edges = thresholdCrossings(time_s, voltage_v, threshold_v)
above = voltage_v > threshold_v;
idx = find(above(2:end) ~= above(1:end-1)) + 1;

if isempty(idx)
    edges = [];
    return;
end

i0 = idx - 1;
i1 = idx;
v0 = voltage_v(i0);
v1 = voltage_v(i1);
t0 = time_s(i0);
t1 = time_s(i1);

den = v1 - v0;
frac = zeros(size(den));
valid = den ~= 0;
frac(valid) = (threshold_v - v0(valid)) ./ den(valid);
frac = max(0, min(1, frac));

edges = t0 + frac .* (t1 - t0);
edges = edges(:);
end

function tc = interpolateCrossing(t0, v0, t1, v1, threshold_v)
if v1 == v0
    frac = 0;
else
    frac = (threshold_v - v0) / (v1 - v0);
end
frac = max(0, min(1, frac));
tc = t0 + frac * (t1 - t0);
end

function [edges, removed] = cancelShortPulses(rawEdges, minIntervalS)
out = zeros(size(rawEdges));
outCount = 0;
removed = 0;

for i = 1:numel(rawEdges)
    edge = rawEdges(i);
    if outCount > 0 && (edge - out(outCount)) < minIntervalS
        outCount = outCount - 1;
        removed = removed + 2;
    else
        outCount = outCount + 1;
        out(outCount) = edge;
    end
end

edges = out(1:outCount);
end

function [burstStart, burstEnd] = splitBursts(edges, gapS)
if numel(edges) < 1
    burstStart = [];
    burstEnd = [];
    return;
end

gapIdx = find(diff(edges) > gapS);
burstStart = [1; gapIdx(:) + 1];
burstEnd = [gapIdx(:); numel(edges)];
end

function figs = makePlots(time_s, voltage_v, cfg, summary, sweep, best, defaultStats, defaultComparator)
figs = gobjects(0);

thr = [sweep.threshold_v];
score = [sweep.score];
fullBursts = [sweep.full_bursts];
longRuns = [sweep.median_long];
removed = [sweep.removed_edges];
duty = [sweep.median_duty_permille];

figs(end + 1) = figure("Name", "OpenVLC analog overview", "Color", "w");
tiledlayout(2, 2, "TileSpacing", "compact");

nexttile;
plotDownsampled(time_s * 1000, voltage_v, cfg.MaxWaveformPoints);
hold on;
yline(best.threshold_v, "g-", sprintf("best %.3f V", best.threshold_v));
yline(cfg.DefaultThresholdV, "r--", sprintf("default %.3f V", cfg.DefaultThresholdV));
grid on;
xlabel("time [ms]");
ylabel("voltage [V]");
title("AGC waveform");

nexttile;
histogram(voltage_v, 160);
hold on;
xline(best.threshold_v, "g-", "best");
xline(cfg.DefaultThresholdV, "r--", "default");
grid on;
xlabel("voltage [V]");
ylabel("samples");
title(sprintf("Voltage distribution, mean %.3f V", summary.mean_v));

nexttile;
plot(thr, score, ".-");
hold on;
xline(best.threshold_v, "g-", "best");
xline(cfg.DefaultThresholdV, "r--", "default");
grid on;
xlabel("threshold [V]");
ylabel("score");
title("Threshold sweep score");

nexttile;
yyaxis left;
plot(thr, longRuns, ".-");
ylabel("median long runs / full burst");
yyaxis right;
plot(thr, fullBursts, ".-");
ylabel("full bursts detected");
grid on;
xlabel("threshold [V]");
title("Long-run damage vs threshold");

figs(end + 1) = figure("Name", "OpenVLC threshold quality", "Color", "w");
tiledlayout(2, 2, "TileSpacing", "compact");

nexttile;
plot(thr, duty / 10, ".-");
hold on;
yline(50, "k--", "50% duty");
xline(best.threshold_v, "g-", "best");
xline(cfg.DefaultThresholdV, "r--", "default");
grid on;
xlabel("threshold [V]");
ylabel("median full-burst duty [%]");
title("Comparator duty estimate");

nexttile;
plot(thr, removed, ".-");
grid on;
xlabel("threshold [V]");
ylabel("removed edges");
title("STM32-like short-pulse cancellation");

nexttile;
bar(categorical(["r1","r2","r3","long"]), ...
    [best.median_r1 best.median_r2 best.median_r3 best.median_long]);
grid on;
ylabel("median count");
title(sprintf("Run bins at best threshold %.3f V", best.threshold_v));

nexttile;
bestEdges = thresholdCrossingsComparator(time_s, voltage_v, best.threshold_v, cfg);
[bestEdges, ~] = cancelShortPulses(bestEdges, cfg.MinIntervalTicks / cfg.TimerHz);
[bs, be] = splitBursts(bestEdges, cfg.GapUs * 1e-6);
burstLens = be - bs + 1;
plot(burstLens, ".-");
grid on;
xlabel("burst index");
ylabel("edges");
title("Burst lengths at best threshold");

if cfg.MaxBurstPlots > 0 && ~isempty(burstLens)
    fullIdx = find(burstLens >= cfg.FullBurstEdges(1) & burstLens <= cfg.FullBurstEdges(2));
    n = min(cfg.MaxBurstPlots, numel(fullIdx));
    if n > 0
        figs(end + 1) = figure("Name", "OpenVLC full-burst intervals", "Color", "w");
        tiledlayout(n, 1, "TileSpacing", "compact");
        for i = 1:n
            idx = fullIdx(i);
            intervals_us = diff(bestEdges(bs(idx):be(idx))) * 1e6;
            nexttile;
            histogram(intervals_us, 120);
            xline(cfg.Run1MaxTicks / cfg.TimerHz * 1e6, "k--", "r1");
            xline(cfg.Run2MaxTicks / cfg.TimerHz * 1e6, "k--", "r2");
            xline(cfg.Run3MaxTicks / cfg.TimerHz * 1e6, "r--", "long");
            grid on;
            xlabel("edge interval [us]");
            ylabel("count");
            title(sprintf("Full burst %d, edges=%d", idx, burstLens(idx)));
        end
    end
end

figs(end + 1) = makeComparatorFigure(time_s, voltage_v, cfg, defaultStats, defaultComparator);
end

function plotDownsampled(x, y, maxPoints)
if numel(x) <= maxPoints
    plot(x, y);
    return;
end

step = ceil(numel(x) / maxPoints);
idx = 1:step:numel(x);
plot(x(idx), y(idx));
end

function fig = makeComparatorFigure(time_s, voltage_v, cfg, defaultStats, defaultComparator)
fig = figure("Name", "STM32 comparator view", "Color", "w");
tiledlayout(4, 1, "TileSpacing", "compact");

tbl = defaultComparator.burstTable;
threshold_v = defaultStats.threshold_v;

if isempty(tbl)
    selectedBurst = [];
    t0 = time_s(1);
    t1 = min(time_s(end), t0 + cfg.ComparatorViewWindowMs * 1e-3);
else
    fullIdx = find(tbl.is_full, 1, "first");
    if isempty(fullIdx)
        [~, fullIdx] = max(tbl.edges_count);
    end
    selectedBurst = fullIdx;
    edgeFirst = defaultComparator.burst_start(selectedBurst);
    edgeLast = defaultComparator.burst_end(selectedBurst);
    center = (defaultComparator.edges(edgeFirst) + defaultComparator.edges(edgeLast)) / 2;
    halfWin = cfg.ComparatorViewWindowMs * 1e-3 / 2;
    t0 = max(time_s(1), center - halfWin);
    t1 = min(time_s(end), center + halfWin);
end

inWindow = time_s >= t0 & time_s <= t1;
t_ms = (time_s(inWindow) - t0) * 1000;
v = voltage_v(inWindow);
digital = comparatorDigitalSamples(v, threshold_v, cfg);

nexttile;
plot(t_ms, v, "b-");
hold on;
yline(defaultComparator.upper_threshold_v, "r--", "upper/threshold");
if cfg.ComparatorHysteresisMV > 0
    yline(defaultComparator.lower_threshold_v, "r--", "lower");
end
grid on;
xlabel("window time [ms]");
ylabel("AGC [V]");
title(sprintf("Analog input and STM32 threshold: %.3f V, DAC=%u, hyst=%g mV", ...
    threshold_v, defaultStats.threshold_dac, cfg.ComparatorHysteresisMV));

nexttile;
stairs(t_ms, double(digital), "k-");
hold on;
edgeMask = defaultComparator.edges >= t0 & defaultComparator.edges <= t1;
edgeTimesMs = (defaultComparator.edges(edgeMask) - t0) * 1000;
if ~isempty(edgeTimesMs)
    plot(edgeTimesMs, 1.10 * ones(size(edgeTimesMs)), "r.", "MarkerSize", 4);
end
ylim([-0.2 1.25]);
grid on;
xlabel("window time [ms]");
ylabel("COMP1_OUT");
title(sprintf("Comparator output seen by TIM2 after thresholding; red dots=edge captures, min pulse=%.3f us", ...
    defaultComparator.min_interval_us));

nexttile;
if ~isempty(selectedBurst)
    edgeFirst = defaultComparator.burst_start(selectedBurst);
    edgeLast = defaultComparator.burst_end(selectedBurst);
    runTicks = diff(defaultComparator.edges(edgeFirst:edgeLast)) * cfg.TimerHz;
    plot(runTicks, ".-");
    hold on;
    yline(cfg.Run1MaxTicks, "k--", "r1");
    yline(cfg.Run2MaxTicks, "k--", "r2");
    yline(cfg.Run3MaxTicks, "r--", "long");
    grid on;
    xlabel("interval index inside selected burst");
    ylabel("run [TIM2 ticks]");
    title(sprintf("Selected burst %d: edges=%d, long=%d, lmax=%.2f us, class=%s", ...
        selectedBurst, tbl.edges_count(selectedBurst), tbl.long(selectedBurst), ...
        tbl.lmax_us(selectedBurst), tbl.decode_class(selectedBurst)));
else
    text(0.1, 0.5, "No burst available");
    axis off;
end

nexttile;
if isempty(tbl)
    text(0.1, 0.5, "No burst table available");
    axis off;
else
    labels = categorical(["good", "marginal", "bad", "not_full"]);
    counts = [sum(tbl.decode_class == "good"), ...
              sum(tbl.decode_class == "marginal"), ...
              sum(tbl.decode_class == "bad"), ...
              sum(tbl.decode_class == "not_full")];
    bar(labels, counts);
    grid on;
    ylabel("burst count");
    est = defaultComparator.decodeEstimate;
    title(sprintf("Decode estimate for STM32 settings: clean %.1f%%, possible %.1f%% of full bursts", ...
        est.estimated_clean_decode_pct, est.estimated_possible_decode_pct));
end
end

function digital = comparatorDigitalSamples(voltage_v, threshold_v, cfg)
if cfg.ComparatorHysteresisMV <= 0
    digital = voltage_v > threshold_v;
    return;
end

upper = threshold_v + cfg.ComparatorHysteresisMV / 2000;
lower = threshold_v - cfg.ComparatorHysteresisMV / 2000;
digital = false(size(voltage_v));
stateHigh = voltage_v(1) > threshold_v;
digital(1) = stateHigh;
for i = 2:numel(voltage_v)
    if stateHigh
        if voltage_v(i) < lower
            stateHigh = false;
        end
    else
        if voltage_v(i) > upper
            stateHigh = true;
        end
    end
    digital(i) = stateHigh;
end
end

function printSummary(csvPath, summary, cfg, best, defaultStats, defaultComparator, sweep)
fprintf("\nOpenVLC analog CSV analysis\n");
fprintf("CSV: %s\n", csvPath);
fprintf("samples=%d duration=%.3f ms dt=%.1f ns fs=%.3f MS/s\n", ...
    summary.samples, summary.duration_s * 1000, summary.dt_s * 1e9, summary.sample_rate_hz / 1e6);
fprintf("voltage min/mean/max/std = %.3f / %.3f / %.3f / %.3f V\n", ...
    summary.min_v, summary.mean_v, summary.max_v, summary.std_v);
fprintf("diff noise std=%.1f mV robust_mad=%.1f mV\n", ...
    summary.diff_std_mv, summary.diff_mad_mv);

fprintf("\nBest threshold by physical edge-stream score:\n");
printThresholdLine(best, cfg);

fprintf("\nConfigured/default threshold comparison:\n");
printThresholdLine(defaultStats, cfg);

fprintf("\nSTM32 comparator simulation at configured/default threshold:\n");
printComparatorEstimate(defaultComparator);

fprintf("\nTop thresholds:\n");
[~, order] = sort([sweep.score], "descend");
for i = 1:min(8, numel(order))
    printThresholdLine(sweep(order(i)), cfg);
end

fprintf("\nSuggested firmware constants for this capture:\n");
fprintf("#define OPENVLC_COMP_THRESHOLD_MV %uu\n", round(best.threshold_v * 1000));
fprintf("#define OPENVLC_COMP_AUTO_MIN_MV %uu\n", max(0, round(best.threshold_v * 1000) - 125));
fprintf("#define OPENVLC_COMP_AUTO_MAX_MV %uu\n", round(best.threshold_v * 1000) + 125);
fprintf("#define OPENVLC_COMP_AUTO_STEP_MV 25u\n");
fprintf("\nInterpretation: long/impossible runs should be low. If best.median_long is still high,\n");
fprintf("the analog AGC/comparator stream is losing transitions; software can only partly recover it.\n\n");
end

function printThresholdLine(s, cfg)
fprintf("thr=%.3f V dac=%u full=%u edges_med=%.0f long_med=%.0f lmax_med=%.2f us duty=%.1f%% removed=%u score=%.1f\n", ...
    s.threshold_v, s.threshold_dac, s.full_bursts, s.median_full_edges, ...
    s.median_long, s.median_lmax_us, s.median_duty_permille / 10, ...
    s.removed_edges, s.score);
if s.full_bursts > 0 && s.median_long > 100
    fprintf("  warning: median long runs >100 at %.3f V; comparator stream is still damaged.\n", s.threshold_v);
end
if s.full_bursts == 0
    fprintf("  warning: no full bursts in expected [%d,%d] edge range.\n", ...
        cfg.FullBurstEdges(1), cfg.FullBurstEdges(2));
end
end

function printComparatorEstimate(comp)
est = comp.decodeEstimate;
fprintf("thr=%.3f V dac=%u hyst=%g mV raw_edges=%u deglitched_edges=%u removed=%u full_bursts=%u\n", ...
    comp.threshold_v, comp.threshold_dac, comp.hysteresis_mv, ...
    numel(comp.raw_edges), numel(comp.edges), comp.removed_edges, est.full_bursts);
fprintf("decode estimate: good=%u marginal=%u bad=%u clean=%.1f%% possible=%.1f%%\n", ...
    est.good_bursts, est.marginal_bursts, est.bad_bursts, ...
    est.estimated_clean_decode_pct, est.estimated_possible_decode_pct);
if est.full_bursts > 0
    fprintf("full-burst median long=%.0f, median lmax=%.2f us -> %s\n", ...
        est.median_full_long, est.median_full_lmax_us, est.note);
else
    fprintf("%s\n", est.note);
end
end

function exportAnalysis(exportDir, csvPath, cfg, summary, sweep, best, defaultStats, defaultComparator, figures)
exportDir = string(exportDir);
if ~isfolder(exportDir)
    mkdir(exportDir);
end

summaryFile = fullfile(exportDir, "openvlc_analog_summary.txt");
fid = fopen(summaryFile, "w");
if fid < 0
    warning("Could not write summary file: %s", summaryFile);
else
    cleaner = onCleanup(@() fclose(fid));
    fprintf(fid, "OpenVLC analog CSV analysis\n");
    fprintf(fid, "CSV: %s\n", csvPath);
    fprintf(fid, "samples=%d\n", summary.samples);
    fprintf(fid, "duration_ms=%.6f\n", summary.duration_s * 1000);
    fprintf(fid, "sample_rate_hz=%.3f\n", summary.sample_rate_hz);
    fprintf(fid, "min_v=%.6f mean_v=%.6f max_v=%.6f std_v=%.6f\n", ...
        summary.min_v, summary.mean_v, summary.max_v, summary.std_v);
    fprintf(fid, "best_threshold_v=%.6f\n", best.threshold_v);
    fprintf(fid, "best_threshold_mv=%u\n", round(best.threshold_v * 1000));
    fprintf(fid, "best_threshold_dac=%u\n", best.threshold_dac);
    fprintf(fid, "default_threshold_v=%.6f\n", defaultStats.threshold_v);
    fprintf(fid, "default_threshold_dac=%u\n", defaultStats.threshold_dac);
    fprintf(fid, "default_raw_edges=%u\n", numel(defaultComparator.raw_edges));
    fprintf(fid, "default_deglitched_edges=%u\n", numel(defaultComparator.edges));
    fprintf(fid, "default_removed_edges=%u\n", defaultComparator.removed_edges);
    fprintf(fid, "default_full_bursts=%u\n", defaultComparator.decodeEstimate.full_bursts);
    fprintf(fid, "default_good_bursts=%u\n", defaultComparator.decodeEstimate.good_bursts);
    fprintf(fid, "default_marginal_bursts=%u\n", defaultComparator.decodeEstimate.marginal_bursts);
    fprintf(fid, "default_bad_bursts=%u\n", defaultComparator.decodeEstimate.bad_bursts);
    fprintf(fid, "default_estimated_clean_decode_pct=%.3f\n", defaultComparator.decodeEstimate.estimated_clean_decode_pct);
    fprintf(fid, "default_estimated_possible_decode_pct=%.3f\n", defaultComparator.decodeEstimate.estimated_possible_decode_pct);
    fprintf(fid, "timer_hz=%.0f min_interval_ticks=%u gap_us=%.3f\n", ...
        cfg.TimerHz, cfg.MinIntervalTicks, cfg.GapUs);
    clear cleaner;
end

sweepFile = fullfile(exportDir, "openvlc_threshold_sweep.csv");
writetable(sweepToTable(sweep), sweepFile);

burstFile = fullfile(exportDir, "openvlc_default_comparator_bursts.csv");
writetable(defaultComparator.burstTable, burstFile);

for i = 1:numel(figures)
    if isgraphics(figures(i))
        pngFile = fullfile(exportDir, sprintf("openvlc_analysis_%02d.png", i));
        saveas(figures(i), pngFile);
    end
end
fprintf("Exported analysis to: %s\n", exportDir);
end

function dac = mvToDac(mv, vrefMv)
dac = round(double(mv) * 4095 / double(vrefMv));
dac = max(0, min(4095, dac));
end

function tbl = sweepToTable(sweep)
tbl = table( ...
    [sweep.threshold_v]', ...
    [sweep.threshold_mv]', ...
    [sweep.threshold_dac]', ...
    [sweep.score]', ...
    [sweep.raw_edges]', ...
    [sweep.edges]', ...
    [sweep.removed_edges]', ...
    [sweep.bursts]', ...
    [sweep.full_bursts]', ...
    [sweep.max_burst_edges]', ...
    [sweep.median_full_edges]', ...
    [sweep.median_duration_ms]', ...
    [sweep.median_r1]', ...
    [sweep.median_r2]', ...
    [sweep.median_r3]', ...
    [sweep.median_long]', ...
    [sweep.median_lmax_ticks]', ...
    [sweep.median_lmax_us]', ...
    [sweep.median_interval_us]', ...
    [sweep.median_duty_permille]', ...
    [sweep.all_duty_permille]', ...
    'VariableNames', { ...
        'threshold_v', ...
        'threshold_mv', ...
        'threshold_dac', ...
        'score', ...
        'raw_edges', ...
        'edges', ...
        'removed_edges', ...
        'bursts', ...
        'full_bursts', ...
        'max_burst_edges', ...
        'median_full_edges', ...
        'median_duration_ms', ...
        'median_r1', ...
        'median_r2', ...
        'median_r3', ...
        'median_long', ...
        'median_lmax_ticks', ...
        'median_lmax_us', ...
        'median_interval_us', ...
        'median_duty_permille', ...
        'all_duty_permille' ...
    });
end

function values = topN(x, n)
if isempty(x)
    values = [];
    return;
end
x = sort(x(:), "descend");
values = x(1:min(n, numel(x)))';
end

function p = percentileLocal(x, q)
x = sort(x(:));
x = x(isfinite(x));
if isempty(x)
    p = NaN(size(q));
    return;
end

q = q(:)';
n = numel(x);
p = zeros(size(q));
for i = 1:numel(q)
    qq = max(0, min(100, q(i)));
    pos = 1 + (n - 1) * qq / 100;
    lo = floor(pos);
    hi = ceil(pos);
    if lo == hi
        p(i) = x(lo);
    else
        w = pos - lo;
        p(i) = x(lo) * (1 - w) + x(hi) * w;
    end
end
end

function m = madLocal(x)
x = x(:);
x = x(isfinite(x));
if isempty(x)
    m = NaN;
    return;
end
med = median(x);
m = median(abs(x - med)) * 1.4826;
end
