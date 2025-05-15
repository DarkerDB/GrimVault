window.addEventListener ('error', (event) => {
  logger.error ('Uncaught error', {
    message: event.message,
    filename: event.filename,
    lineno: event.lineno,
    colno: event.colno,
    error: event.error?.stack || event.error
  });
});

window.addEventListener ('unhandledrejection', (event) => {
  logger.error ('Unhandled promise rejection', {
    reason: event.reason?.stack || event.reason
  });
});