(defun cons-lot (stream char)
  (sleep 2)
  (loop
    (loop repeat 10
      for ar = (make-array (* 1024 1024 1/2) :initial-element 0)
      do (setf (aref ar 1000) 112))
    (format stream "~A" char)
    (finish-output stream)))

(defun threaded-cons-lot (n)
  (loop
    with stream = *standard-output*
    repeat n
    do (sb-thread:make-thread (lambda () (cons-lot stream #\.)))))

(threaded-cons-lot 4)
;(quit)
