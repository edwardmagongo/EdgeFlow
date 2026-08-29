// Applies every .sql file in migrations/ in name order. Deliberately minimal:
// one table and no rollback story does not justify a migration framework.
const fs = require('fs');
const path = require('path');
const { Client } = require('pg');

async function main() {
  const databaseUrl = process.env.DATABASE_URL;
  if (!databaseUrl) {
    console.error('DATABASE_URL is required');
    process.exit(1);
  }

  const directory = path.join(__dirname, '..', 'migrations');
  const files = fs.readdirSync(directory).filter((f) => f.endsWith('.sql')).sort();

  const client = new Client({ connectionString: databaseUrl });
  await client.connect();
  try {
    for (const file of files) {
      const sql = fs.readFileSync(path.join(directory, file), 'utf8');
      await client.query(sql);
      console.log(`applied ${file}`);
    }
  } finally {
    await client.end();
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
