#ifndef STATEMENT_API_H
#define STATEMENT_API_H

int record(int);

#if USE_ROUTE
#define route(value) \
    record(value);
#else
#define route(value) record(-(value));
#endif
#define dispatch(value) route(value)

#endif
