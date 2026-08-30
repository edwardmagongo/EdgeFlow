import './App.css';
import { HealthStrip } from './components/HealthStrip';
import { EventsExplorer } from './components/EventsExplorer';

function App() {
  return (
    <div className="app">
      <header>
        <h1>EdgeFlow Dashboard</h1>
      </header>
      <HealthStrip />
      <EventsExplorer />
    </div>
  );
}

export default App;
