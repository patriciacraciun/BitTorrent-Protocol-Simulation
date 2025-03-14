# BitTorrent-Protocol-Simulation

Tema 2 - PROTOCOLUL BitTorrent

Tema ofera o simulare a unui sistem de tip BitTorrent, implementat cu MPI si
2 tipuri de thread-uri (upload/download) pentru fiecare client. Trackerul
pastreaza informatii despre fisierele din retea (prin structura FileInfo),
iar clientii folosesc structura RequestedFile pentru fisierele pe care doresc
sa le descarce.

Functii Principale
- tracker - aduna informatiile de la fiecare client in add_peer_files_to_tracker.
  Asteapta cereri de tip "SWARM REQUEST" si raspunde cu lista segmentelor si swarmul fisierului.
  Marcheaza peer-ii gata, iar cand toti clientii au finalizat, trimite mesajul "DONE".
- peer - citeste datele din fisier cu process_input_file, apoi lanseaza thread-urile:
  Download Thread: in download_thread_func, apeleaza process_requested_file_by_peer
  pentru fiecare fisier dorit, descarcand segmente.
Upload Thread: in upload_thread_func, raspunde cererilor primite ("SEGMENT REQUEST")
si trimite inapoi hash-ul segmentului.
- process_requested_file_by_peer - apeleaza request_swarm pentru a obtine segments[] si swarm[].
  Parcurge segmentele si, pentru fiecare, invoca request_segment.
  Dupa fiecare 10 segmente descarcate, se reapeleaza request_swarm (pentru actualizare).
  Cand fisierele sunt complete, scrie segmentele cu create_output_file.
- request_segment - alege un peer cu choose_peer_with_least_load, trimite o cerere
  "SEGMENT REQUEST", primeste hash-ul segmentului, apoi trimite "ACK".
  Incrementeaza/decrementeaza incarcarile cu peer_status pentru a balansa cererile.
- request_swarm - trimite "SWARM REQUEST" la tracker, primeste numarul de segmente,
  hash-urile si index-urile, plus swarm_size si lista clientilor participanti.
- choose_peer_with_least_load - cauta in array-ul peer_status peer-ul care are
  active_requests minim, pentru a nu supra-solicita un singur seed.
- add_peer_files_to_tracker - trackerul primeste de la fiecare client lista de fisiere
  detinute si le retine in files[].
  Fiecare client care are macar un segment din fisier este pus in swarm[] corespunzator.
- process_input_file - citeste numele fisierelor, segmentele si hash-urile.
  Trimite datele la tracker cu MPI, pentru a fi inregistrate in baza de date a fisierelor.
