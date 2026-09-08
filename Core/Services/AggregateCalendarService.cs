using WinCal.Core.Models;

namespace WinCal.Core.Services;

/// <summary>
/// 聚合日历服务 —— 合并多个数据源的事件
/// </summary>
public class AggregateCalendarService : ICalendarService, ICalendarUpdateSource, IDisposable
{
    private readonly List<ICalendarService> _services;
    private readonly object _queryLock = new();
    private readonly Dictionary<(DateTime Start, DateTime End), QueryState> _queries = new();
    private (DateTime Start, DateTime End)? _latestQueryKey;

    private static readonly TimeSpan InitialDisplayBudget = TimeSpan.FromMilliseconds(120);
    private static readonly TimeSpan CompletedQueryLifetime = TimeSpan.FromSeconds(5);

    public event EventHandler? EventsChanged;

    public AggregateCalendarService(params ICalendarService[] services)
    {
        _services = services.ToList();
        foreach (var updateSource in _services.OfType<ICalendarUpdateSource>())
            updateSource.EventsChanged += OnChildEventsChanged;
    }

    public async Task<List<CalendarEvent>> GetEventsAsync(DateTime start, DateTime end)
    {
        var key = (start, end);
        QueryState state;
        bool isNewQuery = false;

        lock (_queryLock)
        {
            RemoveExpiredQueries();
            _latestQueryKey = key;
            if (!_queries.TryGetValue(key, out state!))
            {
                state = new QueryState(_services.Count);
                _queries[key] = state;
                isNewQuery = true;
            }
        }

        if (!isNewQuery)
            return state.GetSnapshot();

        if (_services.Count == 0)
            return new List<CalendarEvent>();

        foreach (var service in _services)
            _ = ObserveServiceAsync(service, start, end, key, state);

        // 首屏只给聚合查询一个很短的预算。命中缓存的 ICS 可以先展示，
        // 较慢的系统日历完成后通过 EventsChanged 增量补齐。
        await Task.WhenAny(state.AllCompleted, Task.Delay(InitialDisplayBudget)).ConfigureAwait(false);
        state.MarkInitialResultReturned();
        return state.GetSnapshot();
    }

    private async Task ObserveServiceAsync(
        ICalendarService service,
        DateTime start,
        DateTime end,
        (DateTime Start, DateTime End) key,
        QueryState state)
    {
        var events = await GetEventsSafelyAsync(service, start, end).ConfigureAwait(false);
        if (!state.AddResult(events))
            return;

        bool isLatestQuery;
        lock (_queryLock)
        {
            isLatestQuery = _latestQueryKey == key &&
                            _queries.TryGetValue(key, out var currentState) &&
                            ReferenceEquals(currentState, state);
        }

        if (isLatestQuery)
            EventsChanged?.Invoke(this, EventArgs.Empty);
    }

    private static async Task<List<CalendarEvent>> GetEventsSafelyAsync(
        ICalendarService service, DateTime start, DateTime end)
    {
        try
        {
            return await service.GetEventsAsync(start, end);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(
                $"WinCal: Service {service.GetType().Name} failed: {ex.Message}");
            return new List<CalendarEvent>();
        }
    }

    public async Task<bool> IsAvailableAsync()
    {
        var results = await Task.WhenAll(_services.Select(async service =>
        {
            try { return await service.IsAvailableAsync(); }
            catch { return false; }
        }));
        return results.Any(available => available);
    }

    public async Task<List<CalendarAccountInfo>> GetCalendarAccountsAsync()
    {
        var results = await Task.WhenAll(_services.Select(async service =>
        {
            try { return await service.GetCalendarAccountsAsync(); }
            catch { return new List<CalendarAccountInfo>(); }
        }));
        return results.SelectMany(accounts => accounts).ToList();
    }

    public async Task ForceRefreshAsync()
    {
        await Task.WhenAll(_services.Select(async service =>
        {
            try { await service.ForceRefreshAsync(); }
            catch { }
        }));

        lock (_queryLock)
        {
            _queries.Clear();
            _latestQueryKey = null;
        }
    }

    private void OnChildEventsChanged(object? sender, EventArgs e)
    {
        lock (_queryLock)
        {
            _queries.Clear();
            _latestQueryKey = null;
        }
        EventsChanged?.Invoke(this, EventArgs.Empty);
    }

    private void RemoveExpiredQueries()
    {
        var now = DateTime.UtcNow;
        var expiredKeys = _queries
            .Where(pair => pair.Value.IsCompleted &&
                           now - pair.Value.CompletedAtUtc > CompletedQueryLifetime)
            .Select(pair => pair.Key)
            .ToList();

        foreach (var key in expiredKeys)
            _queries.Remove(key);
    }

    private sealed class QueryState
    {
        private readonly object _lock = new();
        private readonly List<CalendarEvent> _events = new();
        private readonly TaskCompletionSource _allCompleted =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        private int _remaining;
        private bool _initialResultReturned;

        public QueryState(int serviceCount)
        {
            _remaining = serviceCount;
            if (serviceCount == 0)
                _allCompleted.TrySetResult();
        }

        public Task AllCompleted => _allCompleted.Task;
        public bool IsCompleted { get; private set; }
        public DateTime CompletedAtUtc { get; private set; }

        public void MarkInitialResultReturned()
        {
            lock (_lock)
                _initialResultReturned = true;
        }

        public bool AddResult(IEnumerable<CalendarEvent> events)
        {
            lock (_lock)
            {
                _events.AddRange(events);
                _remaining--;
                if (_remaining == 0)
                {
                    IsCompleted = true;
                    CompletedAtUtc = DateTime.UtcNow;
                    _allCompleted.TrySetResult();
                }

                return _initialResultReturned;
            }
        }

        public List<CalendarEvent> GetSnapshot()
        {
            lock (_lock)
            {
                return _events
                    .OrderBy(e => e.StartTime)
                    .ToList();
            }
        }
    }

    public void OpenSystemCalendarApp()
    {
        // 尝试打开第一个可用服务的日历应用
        foreach (var service in _services)
        {
            try
            {
                service.OpenSystemCalendarApp();
                return;
            }
            catch { }
        }
    }

    public void Dispose()
    {
        foreach (var updateSource in _services.OfType<ICalendarUpdateSource>())
            updateSource.EventsChanged -= OnChildEventsChanged;

        foreach (var disposableService in _services.OfType<IDisposable>())
            disposableService.Dispose();

        lock (_queryLock)
        {
            _queries.Clear();
            _latestQueryKey = null;
        }

        EventsChanged = null;
    }
}
