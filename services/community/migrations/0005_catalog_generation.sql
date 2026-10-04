-- Orders catalog publications: each rebuild takes the next generation, and an
-- older generation never overwrites a newer published catalog.
CREATE TABLE IF NOT EXISTS catalog_state (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  generation INTEGER NOT NULL
);
